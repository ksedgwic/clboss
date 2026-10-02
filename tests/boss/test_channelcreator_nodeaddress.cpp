#undef NDEBUG
#include"Boss/Mod/ChannelCreator/NodeAddress.hpp"
#include"Jsmn/Object.hpp"
#include"Net/IPAddr.hpp"
#include"Net/IPAddrOrOnion.hpp"
#include<assert.h>
#include<string>

namespace {

std::unique_ptr<Net::IPAddrOrOnion> lookup(char const* json) {
	return Boss::Mod::ChannelCreator::node_address(
		Jsmn::Object::parse_json(json)
	);
}

}

int main() {
	/* Unknown node.  */
	assert(!lookup(R"({"nodes": []})"));

	/* No node_announcement: the entry has no addresses field.  */
	assert(!lookup(R"({"nodes": [{"nodeid": "02aa"}]})"));

	/* Announced without addresses.  */
	assert(!lookup(R"({"nodes": [{"nodeid": "02aa", "addresses": []}]})"));

	/* Only a dns address.  */
	assert(!lookup(R"({"nodes": [{"nodeid": "02aa", "addresses": [
		{"type": "dns", "address": "eclair.example.net", "port": 9735}
	]}]})"));

	/* A websocket entry, then an ipv4 address: the ipv4 is picked.  */
	auto a = lookup(R"({"nodes": [{"nodeid": "02aa", "addresses": [
		{"type": "websocket", "port": 9736},
		{"type": "ipv4", "address": "203.0.113.7", "port": 9735}
	]}]})");
	assert(a);
	auto ip = Net::IPAddr();
	assert(a->is_ip_addr(ip));
	assert(ip == Net::IPAddr::v4("203.0.113.7"));

	/* A dns address, then a Tor address: the Tor address is picked.  */
	auto o = lookup(R"({"nodes": [{"nodeid": "02aa", "addresses": [
		{"type": "dns", "address": "eclair.example.net", "port": 9735},
		{"type": "torv3", "address": "vww6ybal4bd7szmgncyruucpgfkqahzddi37ktceo3ah7ngmcopnpyyd.onion", "port": 9735}
	]}]})");
	assert(o);
	auto onion = std::string();
	assert(o->is_onion(onion));
	assert(onion == "vww6ybal4bd7szmgncyruucpgfkqahzddi37ktceo3ah7ngmcopnpyyd.onion");

	/* An ipv6 address.  */
	auto six = lookup(R"({"nodes": [{"nodeid": "02aa", "addresses": [
		{"type": "ipv6", "address": "2001:db8::7", "port": 9735}
	]}]})");
	assert(six);
	assert(six->is_ip_addr(ip));
	assert(ip == Net::IPAddr::v6("2001:db8::7"));

	return 0;
}
