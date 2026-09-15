#undef NDEBUG
#include"Sqlite3.hpp"
#include"Ev/Io.hpp"
#include"Ev/concurrent.hpp"
#include"Ev/start.hpp"
#include"Ev/yield.hpp"
#include<assert.h>
#include<stdexcept>
#include<stdlib.h>
#include<unistd.h>

#include<iostream>

int main() {
	auto db = Sqlite3::Db(":memory:");

	auto empty_transaction = [&]() {
		return db.transact().then([&](Sqlite3::Tx tx) {
			tx.commit();
			return Ev::lift();
		});
	};

	auto code = Ev::lift().then([&]() {

		return db.transact();
	}).then([&](Sqlite3::Tx tx) {
		assert(tx);
		tx.commit();
		assert(!tx);

		return db.transact();
	}).then([&](Sqlite3::Tx tx) {
		assert(tx);
		tx.rollback();
		assert(!tx);

		/* Test concurrency.  */
		return Ev::concurrent(empty_transaction());
	}).then([&]() {
		return Ev::concurrent(empty_transaction());
	}).then([&]() {
		return Ev::concurrent(empty_transaction());
	}).then([&]() {
		return Ev::yield();
	}).then([&]() {

		/* Test simple interface.  */
		return db.transact();
	}).then([&](Sqlite3::Tx tx) {
		tx.query_execute("CREATE TABLE \"foo\" (c1 INTEGER, c2 TEXT);");
		tx.commit();

		/* Test full query interface.  */
		return db.transact();
	}).then([&](Sqlite3::Tx tx) {
		auto res = tx.query("INSERT INTO \"foo\" VALUES(:c1, :c2)")
			.bind(":c1", 42)
			.bind(":c2", "some text")
			.execute()
			;
		for (auto& r : res) {
			(void) r;
			/* Should have empty result!  */
			assert(false);
		}
		tx.commit();

		/* Test full query interface again.  */
		return db.transact();
	}).then([&](Sqlite3::Tx tx) {
		auto res = tx.query("SELECT c1, c2 FROM \"foo\"")
			.execute()
			;
		auto flag = false;
		for (auto& r : res) {
			/* Should have single result!  */
			assert(!flag);
			flag = true;
			/* Should be what we inserted.  */
			assert(r.get<int>(0) == 42);
			assert(r.get<std::string>(1) == "some text");
		}
		/* Should have result!  */
		assert(flag);
		tx.commit();

		return Ev::lift(0);
	});

	auto rv = Ev::start(code);
	if (rv != 0)
		return rv;

	/* A lock held by another connection (another process
	 * backing up the file) makes COMMIT fail once the busy
	 * timeout passes.  The failure must reach the caller as an
	 * exception, not end the process, and the database must
	 * serve the next transaction.
	 */
	auto path = std::string("/tmp/test_sqlite3_busy_XXXXXX");
	auto fd = mkstemp(&path[0]);
	assert(fd >= 0);
	close(fd);
	auto writer = Sqlite3::Db(path, 50);
	auto reader = Sqlite3::Db(path, 50);
	auto held = std::make_shared<Sqlite3::Tx>();

	auto busy_code = Ev::lift().then([&]() {
		return writer.transact();
	}).then([&](Sqlite3::Tx tx) {
		tx.query_execute("CREATE TABLE \"bar\" (c1 INTEGER);");
		tx.commit();

		/* The reader starts a transaction and reads, which
		 * holds the file's shared lock until it ends.  */
		return reader.transact();
	}).then([&](Sqlite3::Tx tx) {
		auto res = tx.query("SELECT COUNT(*) FROM \"bar\"")
			.execute();
		for (auto& r : res)
			assert(r.get<int>(0) == 0);
		*held = std::move(tx);

		return writer.transact();
	}).then([&](Sqlite3::Tx tx) {
		tx.query("INSERT INTO \"bar\" VALUES(:c1)")
			.bind(":c1", 1)
			.execute();
		auto threw = false;
		try {
			tx.commit();
		} catch (std::runtime_error const&) {
			threw = true;
		}
		assert(threw);

		/* Release the reader; the writer must be usable.  */
		held->rollback();
		return writer.transact();
	}).then([&](Sqlite3::Tx tx) {
		tx.query("INSERT INTO \"bar\" VALUES(:c1)")
			.bind(":c1", 2)
			.execute();
		tx.commit();

		return reader.transact();
	}).then([&](Sqlite3::Tx tx) {
		/* Only the second insert landed.  */
		auto res = tx.query("SELECT c1 FROM \"bar\"").execute();
		auto n = 0;
		for (auto& r : res) {
			assert(r.get<int>(0) == 2);
			++n;
		}
		assert(n == 1);
		tx.commit();

		return Ev::lift(0);
	});

	rv = Ev::start(busy_code);
	unlink(path.c_str());
	return rv;
}
