#ifndef UTIL_EITHER_HPP
#define UTIL_EITHER_HPP

#include<cstdint>
#include<iostream>
#include<utility>

namespace Util {

/** class Util::Either<L, R>
 *
 * @brief An object that can contain either a type `L`
 * or a type `R`, i.e. a typesafe sum type.
 *
 * @desc We use this instead of `std::variant` as the
 * latter is C++17, and we want to retain C++11 backcompat
 * for now.
 */
template<typename L, typename R>
class Either {
private:
	/* We use a union so that any alignment requirements
	 * are propagated to the top.
	 */
	union U {
		L l;
		R r;
		/* Proper construction and destruction are
		 * handled by the owning `Either` type.
		 */
		U() { }
		~U() { }
	} u;
	bool is_left;

	/* Used by the static constructors to construct
	 * an uninitialized object.
	 */
	struct UnInit { };
	explicit Either(UnInit) { }
public:
	/** Construct a variant that contains the left type.  */
	static
	Either left(L obj) {
		auto rv = Either(UnInit());
		rv.is_left = true;
		new(&rv.u.l) L(std::move(obj));
		return rv;
	}

	/** Construct a variant that contains the right type.  */
	static
	Either right(R obj) {
		auto rv = Either(UnInit());
		rv.is_left = false;
		new(&rv.u.r) R(std::move(obj));
		return rv;
	}

	~Either() {
		if (is_left) {
			u.l.~L();
		} else {
			u.r.~R();
		}
	}

	/** Default-construct a variant of the left type, which
	 * is default-constructed.  */
	Either() {
		is_left = true;
		new(&u.l) L();
	}

	/** Copy construct.  */
	Either(Either const& o) {
		is_left = o.is_left;
		if (is_left) {
			new(&u.l) L(o.u.l);
		} else {
			new(&u.r) R(o.u.r);
		}
	}
	/** Move construct.  */
	Either(Either&& o) {
		is_left = o.is_left;
		if (is_left) {
			new(&u.l) L(std::move(o.u.l));
		} else {
			new(&u.r) R(std::move(o.u.r));
		}
	}

	void swap(Either& o) {
		auto tmp = Either(UnInit());

		/* Move from o to tmp.  */
		tmp.is_left = o.is_left;
		if (tmp.is_left) {
			new(&tmp.u.l) L(std::move(o.u.l));
			o.u.l.~L();
		} else {
			new(&tmp.u.r) R(std::move(o.u.r));
			o.u.r.~R();
		}

		/* Move from this to o.  */
		o.is_left = is_left;
		if (o.is_left) {
			new(&o.u.l) L(std::move(u.l));
			u.l.~L();
		} else {
			new(&o.u.r) R(std::move(u.r));
			u.r.~R();
		}

		/* Move from tmp to this; do not destruct
		 * tmp, as it will be destructed once
		 * we leave this scope.
		 */
		is_left = tmp.is_left;
		if (is_left) {
			new(&u.l) L(std::move(tmp.u.l));
		} else {
			new(&u.r) R(std::move(tmp.u.r));
		}
	}

	/** Copy-assignment.  */
	Either& operator=(Either const& o) {
		if (this == &o)
			return *this;
		/* Construct temporary.  */
		auto tmp = Either(o);
		/* and swap.  */
		swap(tmp);
		return *this;
	}
	/** Move-assignment.  */
	Either& operator=(Either&& o) {
		/* Construct temporary.  */
		auto tmp = Either(std::move(o));
		/* and swap.  */
		swap(tmp);
		return *this;
	}

	/** Inspect the type.  */
	template<typename FL, typename FR>
	void cmatch(FL fl, FR fr) const {
		if (is_left) {
			fl(u.l);
		} else {
			fr(u.r);
		}
	}
	template<typename FL, typename FR>
	void match(FL fl, FR fr) const {
		cmatch(std::move(fl), std::move(fr));
	}
	template<typename FL, typename FR>
	void match(FL fl, FR fr) {
		if (is_left) {
			fl(u.l);
		} else {
			fr(u.r);
		}
	}
};


/** Equality checks for `Either`.  */
template<typename L, typename R>
bool operator==(Util::Either<L,R> const& a, Util::Either<L,R> const& b) {
	auto rv = bool();
	a.match([&](L const& al) {
		b.match([&](L const& bl) {
			rv = (al == bl);
		}, [&](R const& br) {
			rv = false;
		});
	}, [&](R const& ar) {
		b.match([&](L const& bl) {
			rv = false;
		}, [&](R const& br) {
			rv = (ar == br);
		});
	});
	return rv;
}
template<typename L, typename R>
bool operator!=(Util::Either<L,R> const& a, Util::Either<L,R> const& b) {
	return !(a == b);
}

/** Ordering checks for `Either`.  */
template<typename L, typename R>
bool operator<(Util::Either<L,R> const& a, Util::Either<L,R> const& b) {
	auto rv = bool();
	a.match([&](L const& al) {
		b.match([&](L const& bl) {
			rv = (al < bl);
		}, [&](R const& br) {
			/* Arbitrarily order all Left before all Right. */
			rv = true;
		});
	}, [&](R const& ar) {
		b.match([&](L const& bl) {
			rv = false;
		}, [&](R const& br) {
			rv = (ar < br);
		});
	});
	return rv;
}
template<typename L, typename R>
bool operator>(Util::Either<L,R> const& a, Util::Either<L,R> const& b) {
	return b < a;
}
template<typename L, typename R>
bool operator>=(Util::Either<L,R> const& a, Util::Either<L,R> const& b) {
	return !(a < b);
}
template<typename L, typename R>
bool operator<=(Util::Either<L,R> const& a, Util::Either<L,R> const& b) {
	return !(b < a);
}

template<typename L, typename R>
std::ostream& operator<<(std::ostream& os, const Either<L, R>& either) {
    either.cmatch(
        [&](const L& l) {
            os << "Left(" << l << ")";
        },
        [&](const R& r) {
            os << "Right(" << r << ")";
        }
    );
    return os;
}

} // namespace Util

#endif /* !defined(UTIL_EITHER_HPP) */
