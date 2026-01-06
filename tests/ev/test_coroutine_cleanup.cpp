#undef NDEBUG
#include"Ev/coroutine.hpp"
#include"Ev/start.hpp"
#include<assert.h>

/* A coroutine that completes before the caller attaches to the returned Io. */
Ev::Io<int> already_finished() {
	co_return 0;
}

int main() {
	auto io = already_finished();

	/* Simulate the libev idle handler running before anyone consumes the Io.
	 * With the current coroutine cleanup scheduling this destroys the promise
	 * early and the subsequent run would use freed memory.
	 */
	Ev::coroutine::do_cleaning_as_scheduled();

	auto ec = Ev::start(io);
	assert(ec == 0);
	return 0;
}
