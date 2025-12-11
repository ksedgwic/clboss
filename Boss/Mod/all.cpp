#include"Boss/Mod/ActiveProber.hpp"
#include"Boss/Mod/AmountSettingsHandler.hpp"
#include"Boss/Mod/AutoDisconnector.hpp"
#include"Boss/Mod/AvailableRpcCommandsAnnouncer.hpp"
#include"Boss/Mod/BlockTracker.hpp"
#include"Boss/Mod/BoltzSwapper/Main.hpp"
#include"Boss/Mod/ChannelCandidateInvestigator/Main.hpp"
#include"Boss/Mod/ChannelCandidateMatchmaker.hpp"
#include"Boss/Mod/ChannelCandidatePreinvestigator.hpp"
#include"Boss/Mod/ChannelCreateDestroyMonitor.hpp"
#include"Boss/Mod/ChannelCreationDecider.hpp"
#include"Boss/Mod/ChannelCreator/Main.hpp"
#include"Boss/Mod/ChannelFeeManager.hpp"
#include"Boss/Mod/ChannelFeeSetter.hpp"
#include"Boss/Mod/ChannelFinderByDistance.hpp"
#include"Boss/Mod/ChannelFinderByEarnedFee.hpp"
#include"Boss/Mod/ChannelFinderByListpays.hpp"
#include"Boss/Mod/ChannelFinderByPopularity.hpp"
#include"Boss/Mod/ChannelFundsComputer.hpp"
#include"Boss/Mod/ComplainerByLowConnectRate.hpp"
#include"Boss/Mod/ComplainerByLowSuccessPerDay.hpp"
#include"Boss/Mod/ConnectFinderByDns.hpp"
#include"Boss/Mod/ConnectFinderByHardcode.hpp"
#include"Boss/Mod/Connector.hpp"
#include"Boss/Mod/CommandReceiver.hpp"
#include"Boss/Mod/Dowser.hpp"
#include"Boss/Mod/EarningsRebalancer.hpp"
#include"Boss/Mod/EarningsTracker.hpp"
#include"Boss/Mod/FeeModderByBalance.hpp"
#include"Boss/Mod/FeeModderByPriceTheory.hpp"
#include"Boss/Mod/FeeModderBySize.hpp"
#include"Boss/Mod/ForwardFeeMonitor.hpp"
#include"Boss/Mod/FundsMover/Main.hpp"
#include"Boss/Mod/HtlcAcceptor.hpp"
#include"Boss/Mod/InitialConnect.hpp"
#include"Boss/Mod/InitialRebalancer.hpp"
#include"Boss/Mod/Initiator.hpp"
#include"Boss/Mod/InternetConnectionMonitor.hpp"
#include"Boss/Mod/InvoicePayer.hpp"
#include"Boss/Mod/JitRebalancer.hpp"
#include"Boss/Mod/JsonOutputter.hpp"
#include"Boss/Mod/ListfundsAnalyzer.hpp"
#include"Boss/Mod/ListfundsAnnouncer.hpp"
#include"Boss/Mod/ListpaysHandler.hpp"
#include"Boss/Mod/ListpeersAnalyzer.hpp"
#include"Boss/Mod/ListpeersAnnouncer.hpp"
#include"Boss/Mod/Manifester.hpp"
#include"Boss/Mod/MoveFundsCommand.hpp"
#include"Boss/Mod/NeedsConnectSolicitor.hpp"
#include"Boss/Mod/NeedsOnchainFundsSwapper.hpp"
#include"Boss/Mod/NewaddrHandler.hpp"
#include"Boss/Mod/NodeBalanceSwapper.hpp"
#include"Boss/Mod/OnchainFeeMonitor.hpp"
#include"Boss/Mod/OnchainFundsAnnouncer.hpp"
#include"Boss/Mod/OnchainFundsIgnorer.hpp"
#include"Boss/Mod/PaymentDeleter.hpp"
#include"Boss/Mod/PeerCompetitorFeeMonitor/Main.hpp"
#include"Boss/Mod/PeerComplaintsDesk/Main.hpp"
#include"Boss/Mod/PeerFromScidMapper.hpp"
#include"Boss/Mod/PeerMetrician.hpp"
#include"Boss/Mod/PeerStatistician.hpp"
#include"Boss/Mod/RebalanceUnmanager.hpp"
#include"Boss/Mod/Reconnector.hpp"
#include"Boss/Mod/RegularActiveProbe.hpp"
#include"Boss/Mod/RpcWrapper.hpp"
#include"Boss/Mod/SelfUptimeMonitor.hpp"
#include"Boss/Mod/SendpayResultMonitor.hpp"
#include"Boss/Mod/StatusCommand.hpp"
#include"Boss/Mod/SwapManager.hpp"
#include"Boss/Mod/SwapReporter.hpp"
#include"Boss/Mod/TimerTwiceDailyAnnouncer.hpp"
#include"Boss/Mod/Timers.hpp"
#include"Boss/Mod/UnmanagedManager.hpp"
#include"Boss/Mod/Waiter.hpp"
#include"Boss/Mod/all.hpp"
#include"Boss/ModG/TimedBooleanController.hpp"
#include"Boss/Msg/RequestGetAutoOpenFlag.hpp"
#include"Boss/Msg/RequestGetAutoSwapFlag.hpp"
#include"Boss/Msg/RequestGetCircularRebalanceFlag.hpp"
#include"Boss/Msg/ResponseGetAutoOpenFlag.hpp"
#include"Boss/Msg/ResponseGetAutoSwapFlag.hpp"
#include"Boss/Msg/ResponseGetCircularRebalanceFlag.hpp"
#include<vector>

namespace {

class All {
private:
	std::vector<std::shared_ptr<void>> modules;

public:
	template<typename M, typename... As>
	std::shared_ptr<M> install(As&&... as) {
		auto ptr = std::make_shared<M>(as...);
		modules.push_back(std::shared_ptr<void>(ptr));
		return ptr;
	}
};

const char strAutoOpenControllerName[] = "AutoOpenController";
const char strAutoOpenChannelsOption[] = "auto-open-channels";
const char strAutoOpenChannelsDescription[] = "automatic channel opening";

const char strAutoSwapControllerName[] = "AutoSwapController";
const char strAutoSwapOption[] = "auto-swap";
const char strAutoSwapDescription[] = "automatic offchain-to-onchain swap";

const char strCircularRebalanceControllerName[] = "CircularRebalanceController";
const char strCircularRebalanceOption[] = "circular-rebalance";
const char strCircularRebalanceDescription[] = "circular channel rebalancing";

}

namespace Boss { namespace Mod {

std::shared_ptr<void> all( std::ostream& cout
			 , S::Bus& bus
			 , Ev::ThreadPool& threadpool
			 , std::function< Net::Fd( std::string const&
						 , std::string const&
						 )
					> open_rpc_socket
			 ) {
	auto all = std::make_shared<All>();

	/* Basic.  */
	auto waiter = all->install<Waiter>(bus);
	auto imon = all->install<InternetConnectionMonitor>( bus
							   , threadpool
							   , *waiter
							   );
	all->install<JsonOutputter>(cout, bus);
	all->install<CommandReceiver>(bus);
	all->install<RpcWrapper>(bus);
	all->install<AvailableRpcCommandsAnnouncer>(bus);

	/* Startup.  */
	all->install<Manifester>(bus);
	all->install<Initiator>(bus, threadpool, std::move(open_rpc_socket));

	/* General settings.  */
	all->install<AmountSettingsHandler>(bus);

	/* Regular timers.  */
	all->install<BlockTracker>(bus);
	all->install<Timers>(bus, *waiter);
	all->install<TimerTwiceDailyAnnouncer>(bus);

	/* Connection wrangling.  */
	all->install<InitialConnect>(bus);
	all->install<Connector>(bus);
	all->install<NeedsConnectSolicitor>(bus);
	all->install<ConnectFinderByDns>(bus);
	all->install<ConnectFinderByHardcode>(bus);
	all->install<Reconnector>(bus);
	all->install<AutoDisconnector>(bus);

	/* Status monitors.  */
	all->install<ChannelFundsComputer>(bus);
	all->install<ListpeersAnnouncer>(bus);
	all->install<ListpeersAnalyzer>(bus);
	all->install<OnchainFeeMonitor>(bus, *waiter);
	all->install<ListfundsAnnouncer>(bus);
	all->install<ListfundsAnalyzer>(bus);
	all->install<OnchainFundsAnnouncer>(bus);
	all->install<OnchainFundsIgnorer>(bus);
	all->install<ChannelCreateDestroyMonitor>(bus);
	all->install<SelfUptimeMonitor>(bus);

	/* Channel creation wrangling.  */
        all->install<TimedBooleanController<
		Msg::RequestGetAutoOpenFlag, Msg::ResponseGetAutoOpenFlag,
		strAutoOpenControllerName, strAutoOpenChannelsOption,
		strAutoOpenChannelsDescription> >(bus);
        all->install<TimedBooleanController<
		Msg::RequestGetAutoSwapFlag, Msg::ResponseGetAutoSwapFlag,
		strAutoSwapControllerName, strAutoSwapOption,
		strAutoSwapDescription> >(bus);
	all->install<ChannelFinderByDistance>(bus, *waiter);
	all->install<ChannelFinderByEarnedFee>(bus);
	all->install<ChannelFinderByListpays>(bus);
	all->install<ChannelFinderByPopularity>(bus, *waiter);
	all->install<ChannelCandidatePreinvestigator>(bus);
	auto investigator = all->install< ChannelCandidateInvestigator::Main
					>(bus, *imon);
	all->install<ChannelCreationDecider>(bus);
	all->install<ChannelCreator::Main>(bus, *waiter, *investigator);
	all->install<ChannelCandidateMatchmaker>(bus);
	all->install<Dowser>(bus);

	/* Status.  */
	all->install<StatusCommand>(bus);

	/* Offchain-to-onchain swap.  */
	all->install<NewaddrHandler>(bus);
	all->install<BoltzSwapper::Main>(bus, threadpool);
	all->install<SwapManager>(bus);
	all->install<NeedsOnchainFundsSwapper>(bus);
	all->install<NodeBalanceSwapper>(bus);
	all->install<SwapReporter>(bus);

	/* Invoice wrangling.  */
	all->install<InvoicePayer>(bus);
	all->install<ListpaysHandler>(bus);
	all->install<PaymentDeleter>(bus);

	/* Channel fees.  */
	all->install<PeerCompetitorFeeMonitor::Main>(bus);
	all->install<ChannelFeeSetter>(bus);
	all->install<ChannelFeeManager>(bus);
	all->install<FeeModderBySize>(bus);
	all->install<FeeModderByBalance>(bus);
	all->install<FeeModderByPriceTheory>(bus);

	/* HTLC manipulation.  */
	all->install<HtlcAcceptor>(bus, *waiter);

	/* Bad peer monitoring.  */
	all->install<SendpayResultMonitor>(bus);
	all->install<ForwardFeeMonitor>(bus);
	all->install<PeerStatistician>(bus);
	all->install<PeerMetrician>(bus);
	all->install<ActiveProber>(bus, *investigator);
	all->install<RegularActiveProbe>(bus);
	/* Peer complaints.  */
	all->install<PeerComplaintsDesk::Main>(bus);
	all->install<ComplainerByLowConnectRate>(bus);
#ifdef ENABLE_COMPLAINER_BY_LOW_SUCCESS_PER_DAY
	all->install<ComplainerByLowSuccessPerDay>(bus);
#endif /* ENABLE_COMPLAINER_BY_LOW_SUCCESS_PER_DAY */

	/* Channel balancing.  */
        all->install<TimedBooleanController<
		Msg::RequestGetCircularRebalanceFlag,
		Msg::ResponseGetCircularRebalanceFlag,
		strCircularRebalanceControllerName, strCircularRebalanceOption,
		strCircularRebalanceDescription> >(bus);
	all->install<FundsMover::Main>(bus);
	all->install<MoveFundsCommand>(bus);
	all->install<EarningsTracker>(bus);
	all->install<JitRebalancer>(bus);
#ifdef ENABLE_INITIAL_REBALANCER
	all->install<InitialRebalancer>(bus);
#endif /* ENABLE_INITIAL_REBALANCER */
	all->install<EarningsRebalancer>(bus);
	all->install<RebalanceUnmanager>(bus);

	/* Unmanaged nodes.  */
	all->install<UnmanagedManager>(bus);

	/* Utility.  */
	all->install<PeerFromScidMapper>(bus);

	return all;
}

}}
