#include"DnsSeed/Detail/parse_dig_srv.hpp"
#include"DnsSeed/get.hpp"
#include"Ev/Io.hpp"
#include"Ev/ThreadPool.hpp"
#include"Ev/runcmd.hpp"
#include"Ev/yield.hpp"
#include<algorithm>
#include<iterator>
#include<memory>
#include<sstream>

namespace {

template<typename a>
std::string enstring(a const& val) {
	auto os = std::ostringstream();
	os << val;
	return os.str();
}

}

namespace DnsSeed {

Ev::Io<std::string> can_get() {
	return Ev::runcmd("dig", {"-v"}, true).then([](std::string _) {
		return Ev::lift(std::string(""));
	}).catching<std::runtime_error>([](std::runtime_error const& _) {
		return Ev::lift(std::string(
			"`dig` command is not installed, it is usually "
			"available in a `dnsutils` package for your OS."
		));
	});
}

Ev::Io<bool> can_torify( std::string const& seed
		       , std::string const& resolver
		       ) {
	auto at_resolver = std::string("@") + resolver;
	return Ev::runcmd("torify", { "dig", "+tcp"
				    , std::move(at_resolver)
				    , seed, "SRV"
				    }).then([](std::string res) {
		/* If we get 0 results via this path, consider it as
		 * not-torifyable.  */
		auto recs = Detail::parse_dig_srv(std::move(res));
		if (recs.size() == 0)
			return Ev::lift(false);

		return Ev::lift(true);
	}).catching<std::runtime_error>([](std::runtime_error const& _) {
		/* If it errored, consider it not-torifyable.  */
		return Ev::lift(false);
	});
}

Ev::Io<std::vector<std::string>> get( std::string const& seed
				    , std::string const& resolver
				    , bool torify
				    ) {
	/* Some ISPs have default resolvers which do not properly
	 * handle SRV queries.
	 * We thus require a resolver of some kind.
	 */
	auto at_resolver = std::string("@") + resolver;

	auto command = "dig";
	auto args = std::vector<std::string>{ std::move(at_resolver)
					    , seed
					    , "SRV"
					    };
	if (torify) {
		command = "torify";
		args.insert(args.begin(), "+tcp");
		args.insert(args.begin(), "dig");
	}

	return Ev::runcmd( command, std::move(args) 
			 ).then([](std::string res) {
		auto records = Detail::parse_dig_srv(std::move(res));

		/* Generate the connect inputs.  */
		auto rv = std::vector<std::string>();
		std::transform( records.begin(), records.end()
			      , std::back_inserter(rv)
			      , [](DnsSeed::Detail::Record const& r) {
			return r.nodeid + "@" + r.hostname + ":"
			     + enstring(r.port)
			     ;
		});

		return Ev::lift(rv);
	});
}

}
