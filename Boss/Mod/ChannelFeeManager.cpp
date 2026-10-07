#include"Boss/Mod/ChannelFeeManager.hpp"
#include"Boss/Msg/ManifestOption.hpp"
#include"Boss/Msg/Manifestation.hpp"
#include"Boss/Msg/Option.hpp"
#include"Boss/Msg/PeerMedianChannelFee.hpp"
#include"Boss/Msg/ProvideChannelFeeModifier.hpp"
#include"Boss/Msg/ProvideStatus.hpp"
#include"Boss/Msg/SetChannelFee.hpp"
#include"Boss/Msg/SolicitChannelFeeModifier.hpp"
#include"Boss/Msg/SolicitStatus.hpp"
#include"Boss/concurrent.hpp"
#include"Boss/log.hpp"
#include"Ev/Io.hpp"
#include"Ev/map.hpp"
#include"Ev/yield.hpp"
#include"Ln/NodeId.hpp"
#include"S/Bus.hpp"
#include<math.h>
#include<memory>

namespace Boss { namespace Mod {

void ChannelFeeManager::start() {
	bus.subscribe<Msg::Manifestation
		     >([this](Msg::Manifestation const& _) {
		return bus.raise(Msg::ManifestOption{
			"clboss-zerobasefee", Msg::OptionType_String,
			Json::Out::direct(std::string("allow")),
			"Whether to require, allow, or disallow a "
			"0 base fee.  Dynamic: settable at runtime via "
			"`lightning-cli setconfig`.",
			/* dynamic = */ true
		});
	});
	bus.subscribe<Msg::Option
		     >([this](Msg::Option const& o) {
		if (o.name != "clboss-zerobasefee")
			return Ev::lift();

		auto name_of = [](ZeroBaseFee z) {
			return (z == ZeroBaseFee_Require) ? "require" :
			       (z == ZeroBaseFee_Disallow) ? "disallow" :
			       /*otherwise*/ "allow";
		};
		auto svalue = o.value.is_string() ? std::string(o.value)
						  : std::string();
		auto value = ZeroBaseFee_Allow;
		if (svalue == "require" || svalue == "required")
			value = ZeroBaseFee_Require;
		else if (svalue == "allow" || svalue == "allowed")
			value = ZeroBaseFee_Allow;
		else if (svalue == "disallow" || svalue == "disallowed")
			value = ZeroBaseFee_Disallow;
		else {
			/* An unknown word used to mean allow; keep the
			 * current setting instead, and refuse it from
			 * setconfig so it is not persisted.  */
			o.reject( "clboss-zerobasefee: expected require, "
				  "allow or disallow"
				);
			return Boss::log( bus, Boss::Warn
					, "ChannelFeeManager: "
					  "clboss-zerobasefee: expected "
					  "require, allow or disallow; "
					  "keeping %s."
					, name_of(zero_base_fee)
					);
		}
		zero_base_fee = value;
		return Boss::log( bus, Boss::Info
				, "ChannelFeeManager: zerobasefee: %s"
				, name_of(zero_base_fee)
				);
	});

	bus.subscribe<Msg::PeerMedianChannelFee
		     >([this](Msg::PeerMedianChannelFee const& m) {
		return Boss::concurrent(
			perform(m.node, m.base, m.proportional)
		);
	});
	bus.subscribe<Msg::ProvideChannelFeeModifier
		     >([this](Msg::ProvideChannelFeeModifier const& m) {
		if (soliciting)
			modifiers.push_back(m.modifier);
		return Ev::lift();
	});
	bus.subscribe<Msg::SolicitStatus
		     >([this](Msg::SolicitStatus const& _) {
		auto stat = Json::Out();
		auto obj = stat.start_object();
		for (auto const& node_inf : infos) {
			auto const& node = node_inf.first;
			auto const& inf = node_inf.second;
			obj
				.start_object(std::string(node))
					.field( "median_base"
					      , inf.median_base
					      )
					.field( "median_proportional"
					      , inf.median_proportional
					      )
					.field( "multiplier"
					      , inf.multiplier
					      )
					.field( "final_base"
					      , inf.final_base
					      )
					.field( "final_proportional"
					      , inf.final_proportional
					      )
				.end_object();
		}
		obj.end_object();

		return bus.raise(Msg::ProvideStatus{
				"lnfee", std::move(stat)
		});
	});
}
Ev::Io<void> ChannelFeeManager::solicit() {
	/* Busy wait.  */
	if (soliciting && !initted)
		return Ev::yield().then([this]() { return solicit(); });

	if (initted)
		return Ev::lift();

	soliciting = true;
	return bus.raise( Msg::SolicitChannelFeeModifier{}
			).then([this]() {
		soliciting = false;
		initted = true;
		return Ev::lift();
	});
}
Ev::Io<void>
ChannelFeeManager::perform( Ln::NodeId node
			  , std::uint32_t base
			  , std::uint32_t proportional
			  ) {
	/* This message will be mutated later.  */
	auto msg = std::make_shared<Msg::SetChannelFee>();
	msg->node = node;
	msg->base = base;
	msg->proportional = proportional;
	return Ev::lift().then([this]() {
		return solicit();
	}).then([this, msg]() {
		/* Run the modifiers */
		auto f = [msg](std::function<Ev::Io<double>( Ln::NodeId
							   , std::uint32_t
							   , std::uint32_t
							   )
					    > modifier) {
			return modifier( msg->node
				       , msg->base
				       , msg->proportional
				       );
		};
		return Ev::map(std::move(f), modifiers);
	}).then([this, msg](std::vector<double> multipliers) {
		/* Get our info structure.  */
		auto& inf = infos[msg->node];
		inf.median_base = msg->base;
		inf.median_proportional = msg->proportional;

		/* Get double versions of the base and proportional fees.  */
		auto b = double(msg->base);
		auto p = double(msg->proportional);
		auto ms = double(1.0);
		/* Multiply by all, using the `double` versions.  */
		for (auto m : multipliers) {
			b *= m;
			p *= m;
			ms *= m;
		}
		inf.multiplier = ms;
		/* Round and convert back to uint32_t.  */
		msg->base = std::uint32_t(round(b));
		msg->proportional = std::uint32_t(round(p));
		/* Handle zerobasefee.  */
		if (zero_base_fee == ZeroBaseFee_Disallow && msg->base == 0)
			msg->base = 1;
		if (zero_base_fee == ZeroBaseFee_Require)
			msg->base = 0;
		/* If 0, set to 1.  */
		if (msg->proportional == 0)
			msg->proportional = 1;

		/* Get our final base and proportional fees.  */
		inf.final_base = msg->base;
		inf.final_proportional = msg->proportional;

		/* Send the message!  */
		return bus.raise(std::move(*msg));
	});
}

}}
