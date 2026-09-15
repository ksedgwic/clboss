#include"Ev/Io.hpp"
#include"Ev/yield.hpp"
#include"Util/BacktraceException.hpp"
#include"Sqlite3/Db.hpp"
#include"Sqlite3/Tx.hpp"
#include<stdexcept>
#include<queue>
#include<sqlite3.h>

namespace Sqlite3 {

class Db::Impl {
private:
	sqlite3* connection;

	bool in_transaction;
	/* Queue of greenthreads blocked on transact().  */
	std::queue<std::function<void(Sqlite3::Tx)>> blocked;

public:
	Impl(std::string const& filename, unsigned int busy_timeout_ms) {
		in_transaction = false;
		auto res = sqlite3_open(filename.c_str(), &connection);
		if (res != SQLITE_OK) {
			auto msg = std::string();
			if (connection) {
				msg = std::string(sqlite3_errmsg(connection));
				sqlite3_close_v2(connection);
				connection = nullptr;
			} else
				msg = "Not enough memory";
			throw Util::BacktraceException<std::runtime_error>(
				std::string("Sqlite3::Db: sqlite3_open: ") +
				msg
			);
		}
		res = sqlite3_extended_result_codes(connection, 1);
		if (res != SQLITE_OK) {
			auto msg = std::string(sqlite3_errmsg(connection));
			sqlite3_close_v2(connection);
			connection = nullptr;
			throw Util::BacktraceException<std::runtime_error>(
				std::string("Sqlite3::Db: sqlite3_extended_result_codes: ") +
				msg
			);
		}
		res = sqlite3_busy_timeout(connection, int(busy_timeout_ms));
		if (res != SQLITE_OK) {
			auto msg = std::string(sqlite3_errmsg(connection));
			sqlite3_close_v2(connection);
			connection = nullptr;
			throw Util::BacktraceException<std::runtime_error>(
				std::string("Sqlite3::Db: sqlite3_busy_timeout: ") +
				msg
			);
		}
		res = sqlite3_exec( connection, "PRAGMA foreign_keys = ON;"
				  , NULL, NULL, NULL
				  );
		if (res != SQLITE_OK) {
			auto msg = std::string(sqlite3_errmsg(connection));
			sqlite3_close_v2(connection);
			connection = nullptr;
			throw Util::BacktraceException<std::runtime_error>(
				std::string("Sqlite3::Db: "
					    "PRAGMA foreign_keys = ON: "
					   ) +
				msg
			);
		}
	}
	~Impl() {
		if (connection)
			sqlite3_close_v2(connection);
	}

	Ev::Io<Sqlite3::Tx> transact(Db const& db) {
		auto ptx = std::make_shared<Sqlite3::Tx>();
		return Ev::Io< Sqlite3::Tx
			     >([ this, db
			       ]( std::function<void(Sqlite3::Tx)> pass
				, std::function<void(std::exception_ptr)> _
				) {
			if (in_transaction)
				blocked.emplace(std::move(pass));
			else {
				in_transaction = true;
				pass(Sqlite3::Tx(db));
			}
		}).then([ptx](Sqlite3::Tx tx) {
			*ptx = std::move(tx);
			return Ev::yield();
		}).then([ptx]() {
			return Ev::lift(std::move(*ptx));
		});
	}
	void* get_connection() const { return connection; }
	void transaction_finish(Db const& db) {
		if (!blocked.empty()) {
			auto pass = std::move(blocked.front());
			blocked.pop();
			pass(Sqlite3::Tx(db));
		} else
			in_transaction = false;
	}
};

void* Db::get_connection() const {
	return pimpl->get_connection();
}
void Db::transaction_finish() {
	return pimpl->transaction_finish(*this);
}
Ev::Io<Sqlite3::Tx> Db::transact() {
	return pimpl->transact(*this);
}

Db::Db( std::string const& filename
      , unsigned int busy_timeout_ms
      ) : pimpl(std::make_shared<Impl>(filename, busy_timeout_ms)) { }

}
