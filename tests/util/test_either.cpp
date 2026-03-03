#undef NDEBUG
#include"Util/Either.hpp"
#include<assert.h>
#include<cstdint>
#include<functional>
#include<set>
#include<string>

typedef Util::Either<std::uint64_t, std::string> Example;

int main() {
	/* Safe construct / destruct tests.
	 * These are basically only usefully tested under
	 * valgrind.
	 */
	{
		(void) Example();
		(void) Example::left(0);
		(void) Example::right("some long and pointless string");
		(void) Example(Example::right("another string"));
		auto foo = Example::right("yet another string");
		(void) Example(foo);
	}

	/* Equality checks.  */
	{
		assert(Example::left(0) == Example::left(0));
		assert(Example::left(0) != Example::left(42));
		assert(Example::right("hot diggity string") == Example::right("hot diggity string"));
		assert(Example::right("hot diggity string") != Example::right("not a hot diggity string"));
		assert(Example::left(0) != Example::right("some arbitrary string"));

		auto a = Example::left(100);
		auto b = a;
		assert(a == b);
		auto c = Example::right("stringity string");
		auto d = c;
		assert(c == d);
		assert(a != c);
	}

	/* Ordering checks.  */
	{
		assert(!(Example::left(0) < Example::left(0)));
		assert(!(Example::left(0) > Example::left(0)));
		assert(Example::left(0) <= Example::left(0));
		assert(Example::left(0) >= Example::left(0));

		auto a = Example::left(0);
		auto b = Example::left(1);
		auto c = Example::right("asdf fdsa asdf");
		assert(((Example const&)a) < ((Example const&)b));
		assert(std::less<Example>{}(a, b));
		assert(std::less<Example>{}(b, c));
		assert(c > b);
		assert(a <= a);
		assert(a <= b);
		assert(c >= b);
		assert(c >= c);

		auto s = std::set<Example>();
		s.insert(Example::left(0));
		s.insert(Example::left(1));
		s.insert(Example::right("a very long string"));
		s.insert(Example::right("a not very long string"));
		assert(s.find(Example::left(0)) != s.end());
		assert(s.find(Example::left(1)) != s.end());
		assert(s.find(Example::left(2)) == s.end());
		assert(s.find(Example::right("nothing at all like this")) == s.end());
		assert(s.find(Example::right("a very long string")) != s.end());
	}

	/* Swap and safe assignment checks.
	 * Note: Safety requires valgrind to check.
	 */
	{
		auto a = Example::left(0);
		auto b = Example::right("yet another string in my test");
		assert(a < b);
		a.swap(b);
		assert(b < a);
		a = b;
		{ auto& ref = a; a = ref; } // Intentional self-assignment test
		assert(a == b);

		a = Example::left(0);
		b = Example::right("another string another check");
		assert(a != b);
		assert(a == Example::left(0));
		assert(b == Example::right("another string another check"));
		a = std::move(b);
		assert(a == Example::right("another string another check"));
	}
}
