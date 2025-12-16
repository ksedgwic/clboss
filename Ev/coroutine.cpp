#include"Ev/coroutine.hpp"
#include<cassert>
#include<cstdio>
#include<cstdlib>
#include<functional>
#include<thread>
#include<ev.h>

namespace {

/* Take advantage of the fact that `Ev::Io` is
fully intended to be used only in the main thread.
*/
auto cleaning_list = (Ev::coroutine::ToBeCleaned*)(nullptr);

/* To avoid allocating in schedule_for_cleaning, also
statically-allocate the idler object.
*/
auto cleaning_idle = ev_idle();

void cleaning_handler(EV_P_ ev_idle* raw_idler, int) {
	assert(raw_idler == &cleaning_idle);
	/* Tell libev to stop the idling.  */
	ev_idle_stop(EV_A_ &cleaning_idle);
	/* And clean it up, which should reset the
	cleaning list to empty.
	*/
	Ev::coroutine::do_cleaning_as_scheduled();
}

}

namespace Ev::coroutine {

#ifndef NDEBUG
namespace detail {

auto const main_thread_id = std::this_thread::get_id();

[[noreturn]]
void assert_fail_off_main_thread(char const* where) noexcept {
	auto const main_tid = std::hash<std::thread::id>()(main_thread_id);
	auto const curr_tid = std::hash<std::thread::id>()(std::this_thread::get_id());
	std::fprintf(stderr,
		     "%s called off main thread (main tid hash=%zu, current tid hash=%zu).\n"
		     "This is unsafe: it mutates coroutine cleanup state and uses libev watchers without synchronization.\n",
		     where, main_tid, curr_tid);
	std::abort();
}

void assert_main_thread(char const* where) noexcept {
	if (std::this_thread::get_id() != main_thread_id) {
		assert_fail_off_main_thread(where);
	}
}

}
#endif

void schedule_for_cleaning(ToBeCleaned* obj) noexcept {
#ifndef NDEBUG
	detail::assert_main_thread("Ev::coroutine::schedule_for_cleaning");
#endif
	auto need_to_schedule = (cleaning_list == nullptr);
	obj->next = cleaning_list;
	cleaning_list = obj;
	if (!need_to_schedule) {
		return;
	}
	/* Should not fail.  Dunno if libev has to allocate
	here but at least these are C libraries and should
	not *throw* at least (^^;).
	*/
	ev_idle_init(&cleaning_idle, &cleaning_handler);
	ev_idle_start(EV_DEFAULT_ &cleaning_idle);
}

void do_cleaning_as_scheduled() noexcept {
	while (cleaning_list) {
		auto head = cleaning_list;
		auto next = head->next;
		head->next = nullptr;
		cleaning_list = next;
		/* Make a best-effort at cleaning this up.  */
		try {
			head->clean();
		} catch (...) { }
	}
}


}
