#undef NDEBUG
#include"Ev/coroutine.hpp"
#include"Ev/start.hpp"
#include"Ev/yield.hpp"
#include<assert.h>

[[gnu::noinline]]
Ev::Io<int> io_gone_before_finalize() {
	co_await Ev::yield();
	co_return 0;
}

[[gnu::noinline]]
void consume(Ev::Io<int>) { }

int main() {
	consume(io_gone_before_finalize());

	auto pump = Ev::yield(2).then([]() {
		return Ev::lift(0);
	});
	auto ec = Ev::start(std::move(pump));
	assert(ec == 0);

	Ev::coroutine::do_cleaning_as_scheduled();
	return 0;
}

