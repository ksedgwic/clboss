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
	bool rollback;

	void throw_sqlite3(char const* src) {
		auto connection = (sqlite3*) db.get_connection();
		auto err = std::string(sqlite3_errmsg(connection));
		throw Util::BacktraceException<std::runtime_error>(
			std::string("Sqlite3::Tx: ") + src + ": " + err
		);
	}

public:
	Impl(Sqlite3::Db const& db_) : db(db_), rollback(true) {
		auto connection = (sqlite3*) db.get_connection();
		auto res = sqlite3_exec(connection, "BEGIN", NULL, NULL, NULL);
		if (res != SQLITE_OK)
			throw_sqlite3("BEGIN");
	}

	void clear_rollback() { rollback = false; }

	~Impl() {
		auto connection = (sqlite3*) db.get_connection();
		auto cmd = (const char*) nullptr;
		if (rollback)
			cmd = "ROLLBACK";
		else
			cmd = "COMMIT";
		auto res = sqlite3_exec(connection, cmd, NULL, NULL, NULL);
		if (res != SQLITE_OK)
			throw_sqlite3(cmd);
		db.transaction_finish();
	}

	void query_execute(char const* q) {
		auto connection = (sqlite3*) db.get_connection();
		auto res = sqlite3_exec(connection, q, NULL, NULL, NULL);
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
	pimpl->clear_rollback();
	pimpl = nullptr;
}
void Tx::rollback() {
	pimpl = nullptr;
}

Query Tx::query(char const* sql) {
	return pimpl->query(sql);
}

Query Tx::query(std::string const& q) {
	return query(q.c_str());
}

void Tx::query_execute(char const* q) {
	return pimpl->query_execute(q);
}

}
