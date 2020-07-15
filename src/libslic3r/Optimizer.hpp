#ifndef NLOPTOPTIMIZER_HPP
#define NLOPTOPTIMIZER_HPP

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4244)
#pragma warning(disable: 4267)
#endif
#include <nlopt.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <utility>
#include <tuple>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <cassert>

namespace Slic3r { namespace opt {

// A type to hold the complete result of the optimization.
template<size_t N> struct Result {
    int resultcode;
    std::array<double, N> optimum;
    double score;
};

// An interval of possible input values for optimization
class Bound {
    double m_min, m_max;

public:
    Bound(double min = std::numeric_limits<double>::min(),
          double max = std::numeric_limits<double>::max())
        : m_min(min), m_max(max)
    {}

    double min() const noexcept { return m_min; }
    double max() const noexcept { return m_max; }
};

template<size_t N> using Bounds = std::array<Bound, N>;
template<size_t N> Bounds<N> bounds(const Bound (&b) [N]) { return to_arr(b); }

// Helper type for the input (function) values of optimization
template<size_t N> using Input = std::array<double, N>;
template<size_t N> Input<N> initvals(const double (&a) [N]) { return to_arr(a); }

// A type for specifying the stop criteria. Setter methods can be concatenated
class StopCriteria {

    // If the absolute value difference between two scores.
    double m_abs_score_diff = std::nan("");

    // If the relative value difference between two scores.
    double m_rel_score_diff = std::nan("");

    // Stop if this value or better is found.
    double m_stop_score = std::nan("");

    // A predicate that if evaluates to true, the optimization should terminate
    // and the best result found prior to termination should be returned.
    std::function<bool()> m_stop_condition = [] { return false; };

    // The max allowed number of iterations.
    unsigned m_max_iterations = 0;

public:

    StopCriteria & abs_score_diff(double val)
    {
        m_abs_score_diff = val; return *this;
    }

    double abs_score_diff() const { return m_abs_score_diff; }

    StopCriteria & rel_score_diff(double val)
    {
        m_rel_score_diff = val; return *this;
    }

    double rel_score_diff() const { return m_rel_score_diff; }

    StopCriteria & stop_score(double val)
    {
        m_stop_score = val; return *this;
    }

    double stop_score() const { return m_stop_score; }

    StopCriteria & max_iterations(double val)
    {
        m_max_iterations = val; return *this;
    }

    double max_iterations() const { return m_max_iterations; }

    template<class Fn> StopCriteria & stop_condition(Fn &&cond)
    {
        m_stop_condition = cond; return *this;
    }

    bool stop_condition() { return m_stop_condition(); }
};

// Helper to be used in static_assert.
template<class T> struct always_false { enum { value = false }; };

// Basic interface to optimizer object
template<class Method, class Enable = void> class Optimizer {
public:

    Optimizer(const StopCriteria &)
    {
        static_assert(always_false<Method>::value,
                      "Optimizer unimplemented for given method!");
    }

    Optimizer<Method, Enable> &to_min() { return *this; }
    Optimizer<Method, Enable> &to_max() { return *this; }
    Optimizer<Method, Enable> &set_criteria(const StopCriteria &) { return *this; }
    StopCriteria get_criteria() const { return {}; };

    template<class Func, size_t N>
    Result<N> optimize(Func&& func,
                       const Input<N> &initvals,
                       const Bounds<N>& bounds);

    // optional for randomized methods:
    void seed(long /*s*/) {}
};

namespace detail {

// Helper types for NLopt algorithm selection in template contexts
template<nlopt_algorithm alg> struct NLoptAlg {};

// NLopt can combine multiple algorithms if one is global an other is a local
// method. This is how template specializations can be informed about this fact.
template<nlopt_algorithm gl_alg, nlopt_algorithm lc_alg = NLOPT_LN_NELDERMEAD>
struct NLoptAlgComb {};

// Convert any collection to tuple. This is useful for object functions taking
// an argument list of doubles. Make things cleaner on the call site of
// optimize().
template<size_t I, std::size_t N, class T, class C> struct to_tuple_ {
    static auto call(const C &c)
    {
        return std::tuple_cat(std::tuple<T>{c[I]},
                              to_tuple_<I-1, N, T, C>::call(c));
    }
};

template<size_t N, class T, class C> struct to_tuple_<0, N, T, C> {
    static auto call(const C &) { return std::tuple<>{}; }
};

// C array to tuple
template<std::size_t N, class T> auto carray_tuple(const T *v)
{
    return to_tuple_<N, N, T, const T*>::call(v);
}

// Helper to convert C style array to std::array
template<class T, size_t N> auto to_arr(const T (&a) [N])
{
    std::array<T, N> r;
    std::copy(std::begin(a), std::end(a), std::begin(r));
    return r;
}

enum class OptDir { MIN, MAX }; // Where to optimize

struct NLopt { // Helper RAII class for nlopt_opt
    nlopt_opt ptr = nullptr;

    template<class...A> explicit NLopt(A&&...a)
    {
        ptr = nlopt_create(std::forward<A>(a)...);
    }

    NLopt(const NLopt&) = delete;
    NLopt(NLopt&&) = delete;
    NLopt& operator=(const NLopt&) = delete;
    NLopt& operator=(NLopt&&) = delete;

    ~NLopt() { nlopt_destroy(ptr); }
};

template<class Method> class NLoptOpt {};

// Optimizers based on NLopt.
template<nlopt_algorithm alg> class NLoptOpt<NLoptAlg<alg>> {
protected:
    StopCriteria m_stopcr;
    OptDir m_dir;

    template<class Fn> using TData =
        std::tuple<std::remove_reference_t<Fn>*, NLoptOpt*, nlopt_opt>;

    template<class Fn, size_t N>
    static double optfunc(unsigned n, const double *params,
                          double */*gradient*/,
                          void *data)
    {
        assert(n >= N);

        auto tdata = static_cast<TData<Fn>*>(data);

        if (std::get<1>(*tdata)->m_stopcr.stop_condition())
            nlopt_force_stop(std::get<2>(*tdata));

        auto fnptr = std::get<0>(*tdata);
        auto funval = carray_tuple<N>(params);

        return std::apply(*fnptr, funval);
    }

    template<class Func, size_t N>
    void set_up(NLopt &nl, Func&& func, const Bounds<N>& bounds)
    {
        std::array<double, N> lb, ub;

        for (size_t i = 0; i < N; ++i) {
            lb[i] = bounds[i].min();
            ub[i] = bounds[i].max();
        }

        nlopt_set_lower_bounds(nl.ptr, lb.data());
        nlopt_set_upper_bounds(nl.ptr, ub.data());

        double abs_diff = m_stopcr.abs_score_diff();
        double rel_diff = m_stopcr.rel_score_diff();
        double stopval = m_stopcr.stop_score();
        if(!std::isnan(abs_diff)) nlopt_set_ftol_abs(nl.ptr, abs_diff);
        if(!std::isnan(rel_diff)) nlopt_set_ftol_rel(nl.ptr, rel_diff);
        if(!std::isnan(stopval))  nlopt_set_stopval(nl.ptr, stopval);

        if(this->m_stopcr.max_iterations() > 0)
            nlopt_set_maxeval(nl.ptr, this->m_stopcr.max_iterations());

        TData<Func> data = std::make_tuple(&func, this, nl.ptr);

        switch(m_dir) {
        case OptDir::MIN:
            nlopt_set_min_objective(nl.ptr, optfunc<Func, N>, &data); break;
        case OptDir::MAX:
            nlopt_set_max_objective(nl.ptr, optfunc<Func, N>, &data); break;
        }
    }

    template<size_t N>
    Result<N> optimize(NLopt &nl, const Input<N> &initvals)
    {
        Result<N> r;

        r.optimum = initvals;
        r.resultcode = nlopt_optimize(nl.ptr, r.optimum.data(), &r.score);

        return r;
    }

public:

    template<class Func, size_t N>
    Result<N> optimize(Func&& func,
                       const Input<N> &initvals,
                       const Bounds<N>& bounds)
    {
        NLopt nl{alg, N};
        set_up(nl, std::forward<Func>(func), bounds);

        return optimize(nl, initvals);
    }

    explicit NLoptOpt(StopCriteria stopcr = {}) : m_stopcr(stopcr) {}

    void set_criteria(const StopCriteria &cr) { m_stopcr = cr; }
    const StopCriteria &get_criteria() const noexcept { return m_stopcr; }
    void set_dir(OptDir dir) noexcept { m_dir = dir; }

    void seed(long s) { nlopt_srand(s); }
};

template<nlopt_algorithm glob, nlopt_algorithm loc>
class NLoptOpt<NLoptAlgComb<glob, loc>>: public NLoptOpt<NLoptAlg<glob>>
{
    using Base = NLoptOpt<NLoptAlg<glob>>;
public:

    template<class Func, size_t N>
    Result<N> optimize(Func&& func,
                       const Input<N> &initvals,
                       const Bounds<N>& bounds)
    {
        NLopt nl_glob{glob, N}, nl_loc{loc, N};

        Base::set_up(nl_glob, std::forward<Func>(func), bounds);
        Base::set_up(nl_loc, std::forward<Func>(func), bounds);
        nlopt_set_local_optimizer(nl_glob.ptr, nl_loc.ptr);

        return Base::optimize(nl_glob, initvals);
    }

    explicit NLoptOpt(StopCriteria stopcr = {}) : Base{stopcr} {}
};

} // namespace detail;

// Optimizers based on NLopt.
template<class M> class Optimizer<detail::NLoptOpt<M>> {
    detail::NLoptOpt<M> m_opt;

public:

    Optimizer& to_max() { m_opt.set_dir(detail::OptDir::MAX); return *this; }
    Optimizer& to_min() { m_opt.set_dir(detail::OptDir::MIN); return *this; }

    template<class Func, size_t N>
    Result<N> optimize(Func&& func,
                       const Input<N> &initvals,
                       const Bounds<N>& bounds)
    {
        return m_opt.optimize(std::forward<Func>(func), initvals, bounds);
    }

    explicit Optimizer(StopCriteria stopcr = {}) : m_opt(stopcr) {}

    Optimizer &set_criteria(const StopCriteria &cr)
    {
        m_opt.set_criteria(cr); return *this;
    }

    const StopCriteria &get_criteria() const { return m_opt.get_criteria(); }

    void seed(long s) { m_opt.seed(s); }
};

// Predefinded NLopt algorithms that are used in the codebase
using AlgNLoptGenetic = detail::NLoptAlgComb<NLOPT_GN_ESCH>;
using AlgNLoptSubplex = detail::NLoptAlg<NLOPT_LN_SBPLX>;
using AlgNLoptSimplex = detail::NLoptAlg<NLOPT_LN_NELDERMEAD>;

// Helper defs for pre-crafted global and local optimizers that work well.
using DefaultGlobalOptimizer = Optimizer<AlgNLoptGenetic>;
using DefaultLocalOptimizer  = Optimizer<AlgNLoptSubplex>;

}} // namespace Slic3r::opt

#endif // NLOPTOPTIMIZER_HPP
