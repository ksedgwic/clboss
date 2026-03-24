#include"DnsSeed/Detail/decode_bech32_node.hpp"
#include"DnsSeed/Detail/parse_dig_srv.hpp"
#include"Util/Str.hpp"
#include<algorithm>
#include<stdexcept>
#include<sstream>

namespace {

std::vector<std::string> split_by_char(std::string const& s, char c) {
	auto rv = std::vector<std::string>();
	auto it = s.begin();
	while (it != s.end()) {
		auto end = std::find(it, s.end(), c);
		rv.push_back(std::string(it, end));
		if (end == s.end())
			it = end;
		else
			it = end + 1;
	}

	return rv;
}

std::uint16_t parse_port(std::string const& s) {
	auto is = std::istringstream(s);
	auto rv = std::uint16_t();
	is >> rv;
	return rv;
}

}

namespace DnsSeed { namespace Detail {

std::vector<Record> parse_dig_srv(std::string const& out) {
	auto rv = std::vector<Record>();

	auto lines = split_by_char(out, '\n');
	for (auto const& line : lines) {
		/* Skip empty and comment lines.  */
		if (line == "")
			continue;
		if (line[0] == ';')
			continue;

		/* Normalize tabs to spaces so field indices
		 * are consistent across dig output formats.  */
		auto normalized = line;
		std::replace(normalized.begin(), normalized.end(), '\t', ' ');

		auto fields = split_by_char(normalized, ' ');
		fields.erase( std::remove( fields.begin(), fields.end()
					 , ""
					 )
			    , fields.end()
			    );

		/* SRV lines have at least 8 fields:
		 * name TTL IN SRV priority weight port target.
		 * Skip non-SRV records (e.g. SOA in AUTHORITY).  */
		if (fields.size() < 8)
			continue;
		if (fields[3] != "SRV")
			continue;

		try {
			/* Extract port and host fields.  */
			auto port_s = *(fields.end() - 2);
			auto port = parse_port(port_s);
			auto host = *(fields.end() - 1);

			/* Extract nodeid.  */
			auto nodeid_s = std::string(
				  host.begin()
				, std::find(host.begin(), host.end(), '.')
			);
			/* Parse it.  */
			auto nodeid = decode_bech32_node(nodeid_s);
			/* Print as hex.  */
			auto nodeid_hex = Util::Str::hexdump(
				  &nodeid[0]
				, nodeid.size()
			);

			rv.push_back({nodeid_hex, port, host});
		} catch (std::exception&) {
			/* Skip records that fail to parse.  */
			continue;
		}
	}

	return rv;
}

}}
