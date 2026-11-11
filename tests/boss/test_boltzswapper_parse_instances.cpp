#undef NDEBUG
#include"Boss/Mod/BoltzSwapper/ServiceCreator.hpp"
#include<assert.h>
#include<stdexcept>
#include<string>

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

	/* Clearnet and onion entries; the label drops the scheme
	 * and the trailing slash, the endpoint only the slash.  */
	auto v = SC::parse_instances(
		"https://swap-a.example.com, https://swap-b.example.com/ ,"
		"http://abcdefgh.onion/api,http://127.0.0.1:9001"
	);
	assert(v.size() == 4);
	assert(v[0].label == "swap-a.example.com");
	assert(v[0].clearnet == "https://swap-a.example.com");
	assert(v[0].onion == "");
	assert(v[1].label == "swap-b.example.com");
	assert(v[1].clearnet == "https://swap-b.example.com");
	assert(v[2].label == "abcdefgh.onion/api");
	assert(v[2].clearnet == "");
	assert(v[2].onion == "http://abcdefgh.onion/api");
	assert(v[3].label == "127.0.0.1:9001");
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

	return 0;
}
