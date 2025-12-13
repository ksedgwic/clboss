#ifndef EV_COROUTINE_HPP
#define EV_COROUTINE_HPP

#include"Ev/Io.hpp"
#include <coroutine>
#include<exception>
#include<functional>
#include<memory>
#include<utility>

/** Contains support for using `Ev::Io` in C++20
 * coroutines, allowing for convenient syntax
 * without the burden of `.then()`.
 *
 * The contents of this namespace are not supposed
 * to be *explicitly* used by coroutine code; just
 * include this header file and write coroutines
 * directly with `co_await` and `co_return`, and the
 * compiler will instantiate the necessary classes
 * for you.
 *
 * Do ***NOT*** use `co_yield` as there is no
 * equivalent concept in `Ev::Io`; either you
 * `co_await` (== `then`) or you `co_return`
 * (== `Ev::lift`).
 *
 * Note that using coroutines increases memory
 * consumption, but now you can get rid of explicit
 * "context" objects whose sole purpose is just to
 * hold on to some objects for you while you thread a
 * gauntlet of `then` boundaries.
 */
namespace Ev::coroutine {

class ToBeCleaned;

/* Function that schedules an item to be cleaned up.  */
void schedule_for_cleaning(ToBeCleaned*) noexcept;
/* Function used internally by the cleanup procedure.  */
void do_cleaning_as_scheduled() noexcept;

/* Base class for things that need cleaning up.

C++20 assumes that the coroutine return object is the
one responsible for calling `std::coroutine_handle::destroy`,
possibly even before the coroutine ends.
In our context, that would be `Ev::Io`, but we do not want
to modify that because it involves a lot of template
metaprogramming that I would rather not dig into again.

Now, every coroutine has a "final suspend" that it performs
when execution leaves the coroutine (past the end, or by
`co_return`), but that final suspend also *cannot* call
the `destroy` member function because apparently the compiler
still writes some stuff to the coroutine object even after
the final suspend.

What we do instead is that at final suspend, we schedule
the `std::coroutine_handle::destroy` to be called later,
at an idle handler.

Note that the returned `Ev::Io` might be destroyed without ever being
run/attached.  In that case we still need to clean up the coroutine
frame after final suspend, so the `Ev::Io` returned from a coroutine
captures a small lifetime token that lets the promise detect that the
last `Ev::Io` handle has gone away and schedule cleanup once finalized.

This is a singly-linked embedded list because one of the
possible things that can cause something to be cleaned
up is `std::bad_alloc`, a.k.a. out-of-memory.
We thus cannot use something like a
`std::vector<ToBeCleaned*>` to track the items to be
cleaned up later, as inserting a new item to that
structure may cause allocation.
With the singly-linked embedded list, the coroutine
itself has the necessary space to maintain the list
of items to be cleaned up, so no extra allocations are
needed and there is no possibility of *another*
`std::bad_alloc` while we are already cleaning up after
one.
*/
class ToBeCleaned {
private:
	ToBeCleaned* next;

	friend void schedule_for_cleaning(ToBeCleaned*) noexcept;
	friend void do_cleaning_as_scheduled() noexcept;

public:
	ToBeCleaned() : next(nullptr) { }
	virtual void clean() =0;
};

/* Base class for `IoAwaiter` below.  */
class IoAwaiterBase {
protected:
	std::exception_ptr error;

public:
	IoAwaiterBase() : error(nullptr) { }
	bool await_ready() const noexcept { return false; }
};

/* Class for awaiting for another `Ev::Io` via
`co_await` syntax.
The promise constructs this class from some concrete
`Ev::Io` that is the result of the expression after
`co_await`.
*/
template<typename a>
class IoAwaiter : public IoAwaiterBase {
private:
	std::unique_ptr<a> pvalue;
	Ev::Io<a> act;

public:
	IoAwaiter() =delete;
	IoAwaiter( Ev::Io<a> act_
		 ) : IoAwaiterBase()
		   , pvalue(nullptr)
		   , act(std::move(act_))
		   { }
	void await_suspend(std::coroutine_handle<> caller) {
		/* This is arguably a hack, TODO: we should
		probably make IoAwaiter a friend of Ev::Io so
		that IoAwaiter can peek at the `core` function
		directly, which lets us remove some of the
		overhead `run` adds.
		*/
		std::move(act).run([this, caller](a value) {
			try {
				pvalue = std::make_unique<a>(std::move(value));
			} catch (...) {
				error = std::current_exception();
			}
			caller.resume();
		}, [this, caller](std::exception_ptr e) {
			error = e;
			caller.resume();
		});
	}
	a await_resume() {
		if (error) {
			std::rethrow_exception(error);
		}
		return std::move(*pvalue);
	}
};
template<>
class IoAwaiter<void>: public IoAwaiterBase {
private:
	Ev::Io<void> act;

public:
	IoAwaiter() =delete;
	IoAwaiter( Ev::Io<void> act_
		 ) : IoAwaiterBase()
		   , act(std::move(act_))
		   { }
	void await_suspend(std::coroutine_handle<> caller) {
		/* See TODO note above for the generic version.  */
		std::move(act).run([this, caller]() {
			try {
				/* nothing */
			} catch (...) {
				error = std::current_exception();
			}
			caller.resume();
		}, [this, caller](std::exception_ptr e) {
			error = e;
			caller.resume();
		});
	}
	void await_resume() {
		if (error) {
			std::rethrow_exception(error);
		}
	}
};

template<typename a> class Promise;

/* Class for awaiting a coroutine function when it has
terminated.
*/
template<typename a>
class FinalSuspendAwaiter {
public:
	FinalSuspendAwaiter() { }

	/*---- Expected by compiler. ----*/
	bool await_ready() const noexcept { return false; }
	void await_suspend(std::coroutine_handle<Promise<a>> func) noexcept;
	void await_resume() noexcept {
		/* Should never be called; the entire *point*
		of the final-suspend is that the coroutine is
		suspended and never resumed because it is
		*final*.
		*/
		std::terminate();
	}
	/*---- Expected by compiler ends. ----*/
};

class PromiseBase : public ToBeCleaned {
protected:
	typedef std::function<void(std::exception_ptr)> FailF;

private:
	struct CleanupState {
		PromiseBase* promise;
		bool io_gone;
		CleanupState() : promise(nullptr), io_gone(false) { }
	};
	struct IoLifetimeToken {
		std::shared_ptr<CleanupState> state;
		explicit IoLifetimeToken(std::shared_ptr<CleanupState> state_)
			: state(std::move(state_)) { }
		~IoLifetimeToken() {
			state->io_gone = true;
			if (state->promise) {
				state->promise->note_io_gone();
			}
		}
	};

protected:
	std::exception_ptr error;
	FailF fail;
	bool attached;
	bool finalized;
	bool cleanup_scheduled;
	std::shared_ptr<CleanupState> cleanup_state;

	virtual void clear_pass() =0;

	std::shared_ptr<IoLifetimeToken> make_io_lifetime_token() {
		return std::make_shared<IoLifetimeToken>(cleanup_state);
	}

	void note_attached() noexcept {
		attached = true;
		try_schedule_cleanup();
	}
	void note_final_suspend() noexcept {
		finalized = true;
		try_schedule_cleanup();
	}

private:
	void note_io_gone() noexcept {
		try_schedule_cleanup();
	}
	void try_schedule_cleanup() noexcept {
		if (cleanup_scheduled) {
			return;
		}
		if (!finalized) {
			return;
		}
		if (!(attached || cleanup_state->io_gone)) {
			return;
		}
		cleanup_scheduled = true;
		schedule_for_cleaning(this);
	}

	template<typename a> friend class FinalSuspendAwaiter;

public:
	PromiseBase() : ToBeCleaned()
		      , error(nullptr)
		      , fail(nullptr)
		      , attached(false)
		      , finalized(false)
		      , cleanup_scheduled(false)
		      , cleanup_state(std::make_shared<CleanupState>())
		      {
		cleanup_state->promise = this;
	}
	~PromiseBase() {
		cleanup_state->promise = nullptr;
	}

	/*---- Expected by compiler.  ----*/
	/* Called when an exception is caught by the
	compiler at the boundaries of the coroutine
	function.
	*/
	void unhandled_exception() {
		auto e = std::current_exception();
		if (fail) {
			clear_pass();
			std::move(fail)(std::move(e));
			return;
		}
		error = std::move(e);
	}
	/* Called on entry to a coroutine function.

	Ideally this would be `std::suspend_always`,
	but that lets the function return immediately
	with a suspended `Ev::Io`, which can then
	*immediately* have its `run()` member function
	called, without being un-suspended.
	Possibly the correct thing to do would be to
	`resume` in the core function of the returned
	`Ev::Io`, but I have not gotten around to
	experimenting with that and figuring out how
	to combine coroutines with `Ev::Io` is painful
	enough already.
	*/
	std::suspend_never initial_suspend() noexcept {
		return std::suspend_never();
	}
	/* Called on co_await.
	We accept any `Ev::Io` and arrange a waiter for
	it.
	*/
	template<typename a>
	IoAwaiter<a> await_transform(Ev::Io<a> act) {
		return IoAwaiter<a>(std::move(act));
	}
	/*---- Expected by compiler ends.  ----*/
};

template<typename a>
class PromiseMid : public PromiseBase {
private:
	/* From ToBeCleaned.  */
	void clean() override {
		auto handle = std::coroutine_handle<Promise<a>>::from_promise(
			*static_cast<Promise<a>*>(this)
		);
		handle.destroy();
	}

public:
	/*---- Expected by compiler. ----*/
	/* Called on exit from the coroutine, whether by
	exception, reaching the terminating curly brace, or
	`co_return`.
	This is waited on *after* the `return_value`,
	`return_void`, or `unhandled_exception` is called,
	and after the coroutine internal variables have been
	destructed, but apparently the compiler still feels
	the need to poke more bytes at the underlying coroutine
	object even after successful suspension.
	*/
	FinalSuspendAwaiter<a> final_suspend() noexcept {
		return FinalSuspendAwaiter<a>();
	}
	/*---- Expected by compiler ends. ----*/
};

template<typename a>
class Promise : public PromiseMid<a> {
private:
	typedef PromiseBase::FailF FailF;
	typedef std::function<void(a)> PassF;

	std::unique_ptr<a> pvalue;
	PassF pass;

	/* From PromiseBase.  */
	void clear_pass() override {
		pass = nullptr;
	}

public:
	Promise() : PromiseMid<a>()
		  , pvalue(nullptr)
		  , pass(nullptr)
		  { }

	/*---- Expected by compiler. ----*/
	/* Integration of the coroutines to the Ev::Io.  */
	Ev::Io<a> get_return_object() {
		auto lifetime = PromiseBase::make_io_lifetime_token();
		return Ev::Io<a>([this, lifetime
				 ]( PassF f_pass
				  , FailF f_fail
				  ) {
			PromiseBase::note_attached();
			if (pvalue) {
				f_fail = nullptr;
				std::move(f_pass)(std::move(*pvalue));
				return;
			}
			/* For some reason the compiler
			has difficulty figuring out what
			this `error` is.
			*/
			if (PromiseBase::error) {
				f_pass = nullptr;
				std::move(f_fail)(std::move(PromiseBase::error));
				return;
			}
			pass = std::move(f_pass);
			PromiseBase::fail = std::move(f_fail);
		});
	}
	void return_value(a value) {
		if (pass) {
			PromiseBase::fail = nullptr;
			std::move(pass)(std::move(value));
			return;
		}
		pvalue = std::make_unique<a>(
			std::move(value)
		);
	}
	/*---- Expected by compiler ends. ----*/
};

template<>
class Promise<void> : public PromiseMid<void> {
private:
	typedef std::function<void()> PassF;

	bool done;
	PassF pass;

	/* From PromiseBase.  */
	void clear_pass() override {
		pass = nullptr;
	}

public:
	Promise() : PromiseMid<void>()
		  , done(false)
		  , pass(nullptr)
		  { }

	/*---- Expected by compiler. ----*/
	Ev::Io<void> get_return_object() {
		auto lifetime = PromiseBase::make_io_lifetime_token();
		return Ev::Io<void>([this, lifetime
				    ]( PassF f_pass
				     , FailF f_fail
				     ) {
			note_attached();
			if (done) {
				f_fail = nullptr;
				std::move(f_pass)();
				return;
			}
			if (error) {
				f_pass = nullptr;
				std::move(f_fail)(std::move(error));
				return;
			}
			pass = std::move(f_pass);
			fail = std::move(f_fail);
		});
	}
	void return_void() {
		if (pass) {
			fail = nullptr;
			std::move(pass)();
			return;
		}
		done = true;
	}
	/*---- Expected by compiler ends. ----*/
};

template<typename a>
void FinalSuspendAwaiter<a>::await_suspend(std::coroutine_handle<Promise<a>> func) noexcept {
	auto promise = &func.promise();
	promise->note_final_suspend();
}

}

/* Tell the compiler that `Ev::Io<a>` has coroutine
support now.
*/
namespace std {
template<typename a, typename... As>
struct coroutine_traits<Ev::Io<a>, As...> {
	using promise_type = Ev::coroutine::Promise<a>;
};
} // namespace std
#endif /* !defined(EV_COROUTINE_HPP) */
