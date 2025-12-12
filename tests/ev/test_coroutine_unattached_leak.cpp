#undef NDEBUG
#include"Ev/coroutine.hpp"
#include<assert.h>

[[gnu::noinline]]
Ev::Io<int> already_finished_unattached() {
	co_return 0;
}

[[gnu::noinline]]
void consume(Ev::Io<int>) { }

int main() {
	consume(already_finished_unattached());
	Ev::coroutine::do_cleaning_as_scheduled();
	return 0;
}

