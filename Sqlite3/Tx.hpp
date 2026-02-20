#ifndef SQLITE3_TX_HPP
#define SQLITE3_TX_HPP

#include<memory>
#include<string>

namespace Sqlite3 { class Db; }
namespace Sqlite3 { class Query; }

namespace Sqlite3 {

/** class Sqlite3::Tx
 *
 * @brief Represents an ongoing database transaction.
 * Writes will only be committed when the tx is
 * committed.
 *
 * @desc This object is movable but not copyable.
 * When the object is valid and then destructed, it
 * automatically commits all updates.
 *
 * The transaction can be rolled back or committed
 * explicitly by `rollback()` or `commit()` member
 * functions.
 * This will roll back/commit the transaction and
 * also make this object invalid.
 *
 * This has a default constructor which creates an
 * unusable, invalid transaction.
 * The proper way of constructing an `Sqlite3::Tx`
 * is by the `Sqlite3::Db::transact` function.
 */
class Tx {
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

	friend class Sqlite3::Db;

	explicit
	Tx(Sqlite3::Db const&);

public:
	/* Can be default-constructed and moved.  */
	Tx();
	Tx(Tx&&);
	~Tx();

	Tx& operator=(Tx&&);

	/* Check for validity.  */
	operator bool() const { return !!pimpl; }
	bool operator!() const { return !pimpl; }

	/** Sqlite3::Tx::query
	 *
	 * @brief Create a query; provide the query template
	 * and bind any parameters to it, then execute to
	 * retrieve results.
	 */
	Sqlite3::Query query(char const*);
	Sqlite3::Query query(std::string const&);

	/** Sqlite3::Tx::query_execute
	 *
	 * @brief Executes a query.
	 *
	 * @desc There is no opportunity to bind parameters
	 * or check results.
	 * This is primarily intended for e.g. table creation
	 * commands.
	 */
	void query_execute(char const*);
	void query_execute(std::string const& q) {
		query_execute(q.c_str());
	}

	/* Commit and rollback.
	 * Pre-condition: the transaction is valid.
	 * Post-condition: the transaction will be invalid.
	 */
	void commit();
	void rollback();
};

}

#endif /* !defined(SQLITE3_TX_HPP) */
