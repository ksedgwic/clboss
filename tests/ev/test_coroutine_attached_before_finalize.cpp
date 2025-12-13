#undef NDEBUG
#include"Ev/coroutine.hpp"
#include"Ev/start.hpp"
#include"Ev/yield.hpp"
#include<assert.h>

Ev::Io<int> attached_before_finalize() {
	co_await Ev::yield();
	co_return 0;
}

int main() {
	auto io = attached_before_finalize();
	auto ec = Ev::start(io);
	assert(ec == 0);
	Ev::coroutine::do_cleaning_as_scheduled();
	return 0;
}

