#ifndef EV_MAP_HPP
#define EV_MAP_HPP

#include"Ev/Io.hpp"
#include"Ev/concurrent.hpp"
#include"Ev/yield.hpp"
#include"Util/make_unique.hpp"
#include"Util/vector_emplace_back.hpp"
#include<algorithm>
#include<iterator>
#include<memory>
#include<vector>

namespace Ev {

/** Ev::map
 *
 * @brief executes the given function on each
 * item of the input vector, returning an
 * action that provides the results in an
 * output vector.
 *
 * @desc each item in the input is moved and
 * passed to the given function.
 * The given function is executed in new
 * greenthreads, so they will overlap in
 * execution time (though note that this is a
 * greenthread library, so compute resource is
 * always a single CPU; this is only useful if
 * I/O is what is delaying you).
 *
 * If the function raises an exception on any
 * item, then all functions will still complete
 * execution (possibly more than one of them
 * can raise an exception), and one of the
 * exceptions raised will be raised after all
 * the functions have finished.
 *
 * The action will not return until all the
 * parallel executions have completed.
 *
 * See also: `Ev::foreach`.
 */
/* mapIO :: (a -> IO b) -> [a] -> IO [b] */
template<typename f, typename a>
Io<std::vector<typename Detail::IoInner<std::invoke_result_t<f, a>>::type>>
map(f func, std::vector<a> as);

namespace Detail {

/* Executes the functions in parallel.  */
template<typename b>
class MapRun : public std::enable_shared_from_this<MapRun<b>> {
private:
	std::vector<Io<b>> actions;
	std::vector<std::unique_ptr<b>> results;
	std::exception_ptr exc;

	size_t pending;
	std::function<void()> waiter;

	MapRun() =delete;
	MapRun(MapRun const&) =delete;
	MapRun(MapRun&&) =delete;

	explicit
	MapRun( std::vector<Io<b>> actions_
	      ) : actions(std::move(actions_)) {
		results.resize(actions.size());
		exc = nullptr;
		pending = actions.size();
		waiter = nullptr;
	}

public:
	static
	std::shared_ptr<MapRun<b>> create(std::vector<Io<b>> actions) {
		/* Cannot use std::make_shared, constructor is private.  */
		return std::shared_ptr<MapRun<b>>(
			new MapRun<b>(std::move(actions))
		);
	}

	Io<std::vector<b>> run() {
		auto self = MapRun<b>::shared_from_this();
		return self->loop( actions.begin()
				 , results.begin()
				 ).then([self]() {
			return self->wait_completion();
		}).then([self]() {
			return self->extract();
		});
	}
private:
	Io<void> loop( typename std::vector<Io<b>>::const_iterator a_it
		     , typename std::vector<std::unique_ptr<b>>::iterator r_it
		     ) {
		if (a_it == actions.end())
			return Ev::lift();
		return Ev::concurrent( background(a_it, r_it)
				     ).then([]() {
			return Ev::yield();
		}).then([this, a_it, r_it]() {
			return loop(a_it + 1, r_it + 1);
		});
	}
	Io<void> background( typename std::vector<Io<b>>::const_iterator a_it
			   , typename std::vector<std::unique_ptr<b>>::iterator r_it
			   ) {
		return Io<void>([ this
				, a_it
				, r_it
				]( std::function<void()> pass
				 , std::function<void(std::exception_ptr)> fail
				 ) {
			fail = nullptr;
			auto finish = [this, pass]() {
				pass();
				--pending;
				if (pending == 0 && waiter) {
					/* Need to move this out!
					 * waiter is a function that
					 * contains a shared-ptr to the
					 * same MapRun object.
					 * So move it out of the MapRun
					 * to kill the self-pointer.
					 */
					auto my_waiter = std::move(waiter);
					my_waiter();
				}
			};
			a_it->run([finish, r_it](b val) {
				*r_it = Util::make_unique<b>(std::move(val));
				finish();
			}, [this, finish](std::exception_ptr n_exc) {
				exc = n_exc;
				finish();
			});
		});
	}
	Io<void> wait_completion() {
		return Io<void>([this]( std::function<void()> pass
				      , std::function<void(std::exception_ptr)> _
				      ) {
			if (pending == 0)
				return pass();
			waiter = pass;
		});
	}
	Io<std::vector<b>> extract() {
		return Io< std::vector<b>
			 >([this]( std::function<void(std::vector<b>)> pass
				 , std::function<void(std::exception_ptr)> fail
				 ) {
			if (exc) {
				pass = nullptr;
				return fail(exc);
			} else {
				fail = nullptr;
				auto rv = std::vector<b>();
				for (auto& r : results) {
					Util::vector_emplace_back( rv
								 , std::move(
									*r
								   )
								 );
				}
				return pass(std::move(rv));
			}
		});
	}
};

/* Binds and contains the argument of the function, then
 * creates an action which applies the function to the argument
 * later.
 *
 * Otherwise if the function throws immediately, during the loop
 * when we are lifting the input array to an array of actions, it
 * will throw at that point, instead of in parallel.
 */
template<typename b, typename f, typename a>
class MapBind : public std::enable_shared_from_this<MapBind<b, f, a>> {
private:
	std::shared_ptr<f> pfunc;
	a arg;

	MapBind(std::shared_ptr<f> pfunc_, a arg_)
		: pfunc(std::move(pfunc_)), arg(std::move(arg_))
		{ }

public:
	static
	std::shared_ptr<MapBind<b, f, a>>
	create(std::shared_ptr<f> pfunc, a arg) {
		return std::shared_ptr<MapBind<b, f, a>>(
			new MapBind<b, f, a>(std::move(pfunc), std::move(arg))
		);
	}

	Io<b> bind() {
		auto self = MapBind<b, f, a>::shared_from_this();
		return Ev::lift().then([self]() {
			return (*self->pfunc)(std::move(self->arg));
		});
	}
};

}

template<typename f, typename a>
Io<std::vector<typename Detail::IoInner<std::invoke_result_t<f, a>>::type>>
map(f func, std::vector<a> as) {
	using b = typename Detail::IoInner<std::invoke_result_t<f, a>>::type;

	/* Save the function into shared storage.  */
	auto pfunc = std::make_shared<f>(std::move(func));
	/* Construct all the actions.  */
	auto actions = std::vector<Io<b>>();
	actions.reserve(as.size());
	std::transform( as.begin(), as.end()
		      , std::back_inserter(actions)
		      , [pfunc](a& val) {
		auto binder = Detail::MapBind<b, f, a>::create(
			pfunc, std::move(val)
		);
		return binder->bind();
	});
	pfunc = nullptr;

	/* Create the MapRun object.  */
	auto runner = Detail::MapRun<b>::create(std::move(actions));
	return runner->run();
}

}

#endif /* !defined(EV_MAP_HPP) */
