#undef NDEBUG
#include"Boss/Mod/ChannelCandidateInvestigator/Secretary.hpp"
#include"Boss/Msg/ProposeChannelCandidates.hpp"
#include"Ev/Io.hpp"
#include"Ev/start.hpp"
#include"Sqlite3.hpp"
#include<algorithm>
#include<assert.h>

namespace {

auto const A = Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000000");
auto const B = Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000001");
auto const C = Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000002");
auto const D = Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000003");

}

int main() {
	typedef Boss::Msg::ProposeChannelCandidates ProposeChannelCandidates;

	auto db = Sqlite3::Db(":memory:");
	auto s = Boss::Mod::ChannelCandidateInvestigator::Secretary();

	auto code = Ev::lift().then([&]() {
		return db.transact();
	}).then([&](Sqlite3::Tx tx) {
		s.initialize(tx);
		/* Fresh database.  */
		assert(s.get_nonnegative_candidates_count(tx) == 0);
		s.add_candidate( tx
			       , ProposeChannelCandidates{A, B}
			       );
		/* Inserted new candidate with 0 score.  */
		assert(s.get_nonnegative_candidates_count(tx) == 1);
		/* Reinitialize is no-op.  */
		s.initialize(tx);
		assert(s.get_nonnegative_candidates_count(tx) == 1);

		/* Re-insert is no-op.  */
		s.add_candidate( tx
			       , ProposeChannelCandidates{A, B}
			       );
		assert(s.get_nonnegative_candidates_count(tx) == 1);

		/* Insert more.  */
		s.add_candidate( tx
			       , ProposeChannelCandidates{C, D}
			       );
		/* Get for investigation with more than the number of
		 * available should give all.  */
		auto res = s.get_for_investigation(tx, 3);
		assert(res.size() == 2);
		assert(std::any_of( res.begin(), res.end()
				  , [](Ln::NodeId n) { return n == A; }
				  ));
		assert(std::any_of( res.begin(), res.end()
				  , [](Ln::NodeId n) { return n == C; }
				  ));

		/* Changes should persist across committed txes.  */
		tx.commit();
		return db.transact();
	}).then([&](Sqlite3::Tx tx) {
		assert(s.get_nonnegative_candidates_count(tx) == 2);

		/* Get for investigation with less than number available
		 * should limit.  */
		auto res = s.get_for_investigation(tx, 1);
		assert(res.size() == 1);

		/* Change the scores.  */
		s.update_score(tx, A, +1, -6, 24);
		s.update_score(tx, C, -1, -6, 24);
		/* Channeling should return only the the node with positive
		 * score.  */
		auto res2 = s.get_for_channeling(tx);
		assert(res2.size() == 1);
		assert(std::any_of( res2.begin(), res2.end()
				  , [](ProposeChannelCandidates n) {
					return n.proposal == A;
				    }
				  ));

		/* Changing the score of non-candidates should be fine.  */
		s.update_score(tx, B, +1, -6, 24);
		s.update_score(tx, D, +1, -6, 24);
		/* Should still be 2 candidates.  */
		auto res3 = s.get_for_investigation(tx, 999);
		assert(res3.size() == 2);

		/* Too much negative score will drop a candidate.  */
		for (auto i = 0; i < 8; ++i)
			s.update_score(tx, C, -1, -6, 24);
		auto res4 = s.get_for_investigation(tx, 999);
		assert(res4.size() == 1);
		assert(std::any_of( res4.begin(), res4.end()
				  , [](Ln::NodeId n) { return n == A; }
				  ));

		tx.commit();
		return db.transact();
	}).then([&](Sqlite3::Tx tx) {

		/* Explicit remove.  */
		s.add_candidate( tx
			       , ProposeChannelCandidates{C, D}
			       );
		s.remove_candidate(tx, A);
		/* Remove of nonexistent candidate should be no-op.  */
		s.remove_candidate(tx, D);
		/* Should be 1 candidate. */
		auto res = s.get_for_investigation(tx, 999);
		assert(res.size() == 1);
		assert(std::any_of( res.begin(), res.end()
				  , [](Ln::NodeId n) { return n == C; }
				  ));
		/* Explicit remove.  */
		s.remove_candidate(tx, C);
		auto res2 = s.get_for_investigation(tx, 999);
		assert(res2.size() == 0);

		tx.commit();
		return db.transact();
	}).then([&](Sqlite3::Tx tx) {

		/* When no candidate has a positive score, channeling
		 * should fall back to returning all candidates.  */
		s.add_candidate( tx
			       , ProposeChannelCandidates{A, B}
			       );
		s.add_candidate( tx
			       , ProposeChannelCandidates{C, D}
			       );
		s.update_score(tx, C, -1, -6, 24);
		auto res = s.get_for_channeling(tx);
		assert(res.size() == 2);
		assert(std::any_of( res.begin(), res.end()
				  , [](ProposeChannelCandidates n) {
					return n.proposal == A;
				    }
				  ));
		assert(std::any_of( res.begin(), res.end()
				  , [](ProposeChannelCandidates n) {
					return n.proposal == C;
				    }
				  ));

		tx.commit();

		return Ev::lift(0);
	});

	return Ev::start(code);
}
