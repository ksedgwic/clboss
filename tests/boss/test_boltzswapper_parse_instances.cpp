#undef NDEBUG
#include"Boss/Mod/BoltzSwapper/ServiceCreator.hpp"
#include<assert.h>
#include<stdexcept>
#include<string>
#include<vector>

namespace {

typedef Boss::Mod::BoltzSwapper::ServiceCreator SC;

bool rejects(std::string const& s) {
	try {
		(void) SC::parse_instances(s);
	} catch (std::invalid_argument const&) {
		return true;
	}
	return false;
}

}

int main() {
	/* Nothing listed.  */
	assert(SC::parse_instances("").empty());
	assert(SC::parse_instances(" , ,").empty());

	/* Clearnet and onion entries; the label is the endpoint,
	 * which drops the trailing slash.  */
	auto v = SC::parse_instances(
		"https://swap-a.example.com, https://swap-b.example.com/ ,"
		"http://abcdefgh.onion/api,http://127.0.0.1:9001"
	);
	assert(v.size() == 4);
	assert(v[0].label == "https://swap-a.example.com");
	assert(v[0].clearnet == "https://swap-a.example.com");
	assert(v[0].onion == "");
	assert(v[1].label == "https://swap-b.example.com");
	assert(v[1].clearnet == "https://swap-b.example.com");
	assert(v[2].label == "http://abcdefgh.onion/api");
	assert(v[2].clearnet == "");
	assert(v[2].onion == "http://abcdefgh.onion/api");
	assert(v[3].label == "http://127.0.0.1:9001");
	assert(v[3].clearnet == "http://127.0.0.1:9001");

	/* A host merely containing "onion" is clearnet.  */
	auto w = SC::parse_instances("https://onion.example.com");
	assert(w.size() == 1 && w[0].onion == "");

	/* Not URLs, and duplicates.  */
	assert(rejects("swap-a.example.com"));
	assert(rejects("ftp://swap-a.example.com"));
	assert(rejects("https://"));
	assert(rejects("https://swap a.example.com"));
	assert(rejects("https://swap-a.example.com,https://swap-a.example.com/"));

	/* plan(): which instances run, and which take new swaps.  */
	typedef Boss::Mod::BoltzSwapper::Instance Instance;
	auto const builtin = std::vector<Instance>
		{ {"old-label", "https://api.example.org", "http://exampleorg.onion/api"}
		, {"second", "", "http://second.onion"}
		};
	auto const labels = [](std::vector<SC::Planned> const& ps) {
		auto rv = std::string();
		for (auto const& p : ps)
			rv += p.instance.label + (p.quoting ? "+ " : "- ");
		return rv;
	};
	auto unknown = std::vector<std::string>();

	/* Nothing configured: the built-in ones.  */
	assert(labels(SC::plan(builtin, false, {}, {}, unknown))
	       == "old-label+ second+ ");
	assert(labels(SC::plan(builtin, false, {}, {"old-label"}, unknown))
	       == "old-label+ second+ ");

	/* A configured entry naming an endpoint of a built-in
	 * instance is that instance by its label, reached by the
	 * endpoint listed and not by its other one.  */
	auto same = SC::plan( builtin, true
			    , SC::parse_instances("https://api.example.org")
			    , {}, unknown
			    );
	assert(labels(same) == "old-label+ ");
	assert(same[0].instance.clearnet == "https://api.example.org");
	assert(same[0].instance.onion == "");
	auto by_onion = SC::plan( builtin, true
				, SC::parse_instances("http://exampleorg.onion/api")
				, {}, unknown
				);
	assert(labels(by_onion) == "old-label+ ");
	assert(by_onion[0].instance.clearnet == "");
	assert(by_onion[0].instance.onion == "http://exampleorg.onion/api");
	/* Both endpoints listed are the one instance with both.  */
	auto both = SC::plan( builtin, true
			    , SC::parse_instances("http://exampleorg.onion/api,https://api.example.org")
			    , {}, unknown
			    );
	assert(labels(both) == "old-label+ ");
	assert(both[0].instance.clearnet == "https://api.example.org");
	assert(both[0].instance.onion == "http://exampleorg.onion/api");
	assert(labels(SC::plan( builtin, true
			      , SC::parse_instances("http://second.onion,http://exampleorg.onion/api")
			      , {}, unknown
			      )) == "second+ old-label+ ");

	/* A service left out of the list, with swaps in flight,
	 * still runs, for those swaps alone: a built-in one by its
	 * label, one configured before by the URL its label is.  */
	auto const only_new = SC::parse_instances("https://new.example.com");
	assert(labels(SC::plan(builtin, true, only_new, {}, unknown))
	       == "https://new.example.com+ ");
	auto kept = SC::plan( builtin, true, only_new
			    , { "old-label", "https://gone.example.net"
			      , "https://new.example.com", "http://gone.onion"
			      }
			    , unknown
			    );
	assert(labels(kept) == "https://new.example.com+ old-label- "
			       "https://gone.example.net- http://gone.onion- ");
	assert(kept[1].instance.clearnet == "https://api.example.org");
	assert(kept[2].instance.clearnet == "https://gone.example.net");
	assert(kept[3].instance.clearnet == "");
	assert(kept[3].instance.onion == "http://gone.onion");
	assert(unknown.empty());

	/* A list that did not parse: no service takes swaps, and
	 * those in flight are still seen through.  */
	assert(labels(SC::plan(builtin, true, {}, {"second"}, unknown))
	       == "second- ");

	/* A label that says nothing of where the service is.  */
	assert(labels(SC::plan( builtin, true, only_new
			      , {"somewhere", "https://a.example,https://b.example"}
			      , unknown
			      )) == "https://new.example.com+ ");
	assert(unknown.size() == 2);
	assert(unknown[0] == "somewhere");

	return 0;
}
