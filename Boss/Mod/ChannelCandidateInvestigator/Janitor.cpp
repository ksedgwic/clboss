#include"Boss/Mod/ChannelCandidateInvestigator/Janitor.hpp"
#include"Boss/Mod/ChannelCandidateInvestigator/Secretary.hpp"
#include"Boss/log.hpp"
#include"Ev/Io.hpp"
#include"Ev/map.hpp"
#include"Ln/Amount.hpp"

namespace Boss { namespace Mod { namespace ChannelCandidateInvestigator {

Janitor::Janitor(S::Bus& bus_)
	: bus(bus_)
	, dowser(bus_)
	{ }

Ev::Io<void>
Janitor::clean_up( Secretary& secretary
		 , std::shared_ptr<Sqlite3::Tx> tx
		 , Ln::Amount min_channel
		 ) {
	auto candidates = secretary.get_all_with_patrons(*tx);

	auto f = [ this
		 , min_channel
		 ](std::pair<Ln::NodeId, Ln::NodeId> candidate_patron) {
		auto const& candidate = candidate_patron.first;
		auto const& patron = candidate_patron.second;
		return dowser.execute(Msg::RequestDowser{
			nullptr, candidate, patron, min_channel
		}).then([candidate, min_channel](Msg::ResponseDowser m) {
			/* If below, return the node ID of the candidate.  */
			if (m.amount < min_channel)
				return Ev::lift(std::make_pair(candidate, m.amount));
			/* Otherwise return a null NodeId, meaning the
			 * candidate should be retained.  */
			return Ev::lift(std::make_pair(Ln::NodeId(), m.amount));
		});
	};
	return Ev::map(f, std::move(candidates)
		      ).then([ this, &secretary, tx
			     ](std::vector<std::pair< Ln::NodeId
						    , Ln::Amount
						    >> candidates_amounts) {
		auto msg = std::string();

		auto first = true;
		for (auto const& c_a : candidates_amounts) {
			auto const& candidate = c_a.first;
			auto const& amount = c_a.second;

			/* Null NodeIds are skipped.  */
			if (!candidate)
				continue;
			if (first)
				first = false;
			else
				msg += ", ";

			msg += std::string(candidate)
			     + "("
			     + std::string(amount)
			     + ")"
			     ;

			secretary.remove_candidate(*tx, candidate);
		}
		/* Say nothing if not removed anything.  */
		if (first)
			return Ev::lift();

		return Boss::log( bus, Debug
				, "ChannelCandidateInvestigator: Janitor: "
				  "Removed: %s"
				, msg.c_str()
				);
	});
}

Ev::Io<bool> Janitor::check_acceptable( Ln::NodeId proposal
				      , Ln::NodeId patron
				      , Ln::Amount min_channel
				      ) {
	return dowser.execute(Msg::RequestDowser{
		nullptr, proposal, patron, min_channel
	}).then([this, proposal, min_channel](Msg::ResponseDowser m) {
		auto act = Ev::lift();
		auto rv = true;
		if (m.amount < min_channel) {
			act += Boss::log( bus, Debug
					, "ChannelCandidateInvestigator: "
					  "Janitor: Rejected %s (%s)"
					, std::string(proposal).c_str()
					, std::string(m.amount).c_str()
					);
			rv = false;
		}
		return std::move(act).then([rv]() {
			return Ev::lift(rv);
		});
	});
}

}}}
