#include"Sqlite3/Db.hpp"
#include"Sqlite3/Query.hpp"
#include"Sqlite3/Tx.hpp"
#include"Util/BacktraceException.hpp"
#include"Util/make_unique.hpp"
#include<stdexcept>
#include<sqlite3.h>

namespace Sqlite3 {

class Tx::Impl {
private:
	Sqlite3::Db db;
	/* BEGIN succeeded and neither COMMIT nor ROLLBACK has run
	 * since.  */
	bool active;
	/* The next greenthread waiting on Db::transact has been let
	 * go.  */
	bool released;

	int exec(char const* cmd) {
		auto connection = (sqlite3*) db.get_connection();
		return sqlite3_exec(connection, cmd, NULL, NULL, NULL);
	}
	std::string errmsg() {
		auto connection = (sqlite3*) db.get_connection();
		return std::string(sqlite3_errmsg(connection));
	}
	void throw_sqlite3(char const* src) {
		throw Util::BacktraceException<std::runtime_error>(
			std::string("Sqlite3::Tx: ") + src + ": " + errmsg()
		);
	}
	/* Runs from the destructor as well, so it must not throw.  */
	void release() {
		if (released)
			return;
		released = true;
		try {
			db.transaction_finish();
		} catch (...) { }
	}

public:
	Impl(Sqlite3::Db const& db_)
		: db(db_), active(false), released(false) {
		auto res = exec("BEGIN");
		if (res != SQLITE_OK) {
			release();
			throw_sqlite3("BEGIN");
		}
		active = true;
	}
	~Impl() {
		/* A ROLLBACK that fails has nothing left to undo: when
		 * sqlite has already ended the transaction it reports
		 * that none is active.  Nothing to act on, and a
		 * destructor cannot throw.  */
		if (active) {
			exec("ROLLBACK");
			active = false;
		}
		release();
	}
	void commit() {
		auto res = exec("COMMIT");
		if (res != SQLITE_OK) {
			auto err = errmsg();
			/* A COMMIT refused for a lock (another process
			 * reading the file for longer than the busy
			 * timeout) leaves the transaction open.  End it
			 * so the connection can serve the next
			 * transaction, then report.  */
			exec("ROLLBACK");
			active = false;
			release();
			throw Util::BacktraceException<std::runtime_error>(
				std::string("Sqlite3::Tx: COMMIT: ") + err
			);
		}
		active = false;
		release();
	}
	void query_execute(char const* q) {
		auto res = exec(q);
		if (res != SQLITE_OK)
			throw_sqlite3(q);
	}
	Query query(char const* sql) {
		auto connection = (sqlite3*) db.get_connection();
		auto stmt = (sqlite3_stmt*) nullptr;
		auto res = sqlite3_prepare_v2( connection, sql, -1
					     , &stmt, nullptr
					     );
		if (res != SQLITE_OK)
			throw_sqlite3(sql);
		return Query(db, stmt);
	}
};

Tx::Tx(Sqlite3::Db const& db)
		: pimpl(Util::make_unique<Impl>(db)) { }
Tx::Tx() : pimpl(nullptr) { }
Tx::Tx(Tx&& o) : pimpl(std::move(o.pimpl)) { }
Tx::~Tx() { }
Tx& Tx::operator=(Tx&& o) {
	auto tmp = std::move(o);
	std::swap(pimpl, tmp.pimpl);
	return *this;
}

void Tx::commit() {
	/* The object is invalid after commit() whether or not the
	 * commit succeeded: on failure the transaction is already
	 * ended and the database released, and a later query would
	 * run outside any transaction.  */
	auto p = std::move(pimpl);
	p->commit();
}
void Tx::rollback() {
	pimpl = nullptr;
}

Query Tx::query(char const* sql) {
	return pimpl->query(sql);
}
void Tx::query_execute(char const* q) {
	return pimpl->query_execute(q);
}

}
