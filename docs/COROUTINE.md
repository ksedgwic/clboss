# Coroutine Pitfalls

## Temporary Lifetime Pitfall (GCC 11/12)

This note is for developers working on CLBOSS.  It summarizes a
coroutine-related pitfall we hit and the mitigation pattern we
adopted.

### Rule of Thumb
Do **not** put aggregate temporaries directly inside a `co_await`
expression. Preconstruct them as locals, then `co_await` using the
local variable.

### Compiler Issue
- GCC PR 107288	https://gcc.gnu.org/bugzilla/show_bug.cgi?id=107288

### Why This Helps
Named locals have unambiguous lifetime across suspend/resume points.
This avoids the GCC bug pattern where the compiler incorrectly manages
the temporary lifetime of the aggregate constructed inside the
`co_await` expression.

### Toolchain Observed
- GCC 11.4.0 (Ubuntu 22.04), `-std=gnu++20`
- ASAN: `-fsanitize=address`, `-O1 -g -fno-omit-frame-pointer`

### Wrong -> Right (Patch Example)
```diff
--- a/Boss/Mod/ChannelFeeSetter.cpp
+++ b/Boss/Mod/ChannelFeeSetter.cpp
@@
-    co_await bus.raise(Msg::MonitorFeeSetChannel{
-        node,
-        base,
-        proportional
-    });
+    Msg::MonitorFeeSetChannel msg{node, base, proportional};
+    co_await bus.raise(std::move(msg));
```

## Lambda Capture Lifetime Pitfall (Compiler-Specific)

This is a separate (but similar) coroutine pitfall reported by
ZmnSCPxj.  Some compilers incorrectly handle coroutine lambdas and
their capture lifetimes.

### Rule of Thumb
Avoid *anonymous coroutine lambdas* entirely. Prefer **named functions**
(member functions are fine) when writing coroutines.

### Symptoms / Failure Mode
Some compilers appear to assume the lambda object itself outlives the
coroutine. They then destroy the lambda object once the next `Ev::Io`
completes, which causes *all captures* to die after the first
`co_await`. That includes reference captures (`&foo`) and `this`, since
the storage for those references is owned by the lambda object.

### Safe Patterns
- For `bus.subscribe`, do *not* write a coroutine lambda directly.
  Call a named coroutine member function instead.
- For `.then(...)` chains, do *not* pass coroutine lambdas. The whole
  point of coroutines is to avoid the `then` pyramid anyway.
- If a function wants an `Ev::Io` (e.g., `Boss::concurrent`) and you
  need a longer coroutine body, implement a named coroutine and call it.

### Extra Safety
Even with named coroutine functions, copy everything you need from any
message *before* the first `co_await` (this is already part of the
coroutine contract we follow elsewhere).
