#define BOOST_TEST_MODULE KernelGenerator
#include <boost/test/unit_test.hpp>
#include "context_setup.hpp"

using namespace vex;

template <class state_type>
void sys_func(const state_type &x, state_type &dx, double dt) {
    dx = dt * sin(x);
}

template <class state_type, class SysFunction>
void runge_kutta_4(SysFunction sys, state_type &x, double dt) {
    state_type xtmp, k1, k2, k3, k4;

    sys(x, k1, dt);

    xtmp = x + 0.5 * k1;
    sys(xtmp, k2, dt);

    xtmp = x + 0.5 * k2;
    sys(xtmp, k3, dt);

    xtmp = x + k3;
    sys(xtmp, k4, dt);

    x += (k1 + 2 * k2 + 2 * k3 + k4) / 6;
}

BOOST_AUTO_TEST_CASE(kernel_generator)
{
    typedef vex::generator::symbolic<double> sym_state;

    const size_t n  = 1024;
    const double dt = 0.01;

    std::ostringstream body;
    vex::generator::set_recorder(body);

    sym_state sym_x(sym_state::VectorParameter);

    // Record expression sequience.
    runge_kutta_4(sys_func<sym_state>, sym_x, dt);

    // Build kernel.
    auto kernel = vex::generator::build_kernel(
            ctx, "rk4_stepper", body.str(), sym_x);

    std::vector<double> x = random_vector<double>(n);
    vex::vector<double> X(ctx, x);

    for(int i = 0; i < 100; i++) kernel(X);

    check_sample(X, [&](size_t idx, double a) {
            double s = x[idx];
            for(int i = 0; i < 100; i++)
                runge_kutta_4(sys_func<double>, s, dt);

            BOOST_CHECK_CLOSE(a, s, 1e-8);
            });
}

/*
An alternative variant, which does not use the generator facility.
Intermediate subexpression are cuptured with help of 'auto' keyword, and
are combined into larger expression.

This is not as effective as generated kernel, because same input vector
(here 'x') is passed as several different parameters. This specific example
takes about twice as long to execute as the above variant.

Nevertheless, this may be more convenient in some cases.
*/
BOOST_AUTO_TEST_CASE(lazy_evaluation)
{
    const size_t n  = 1024;
    const double dt = 0.01;

    auto rk4 = [](vex::vector<double> &x, double dt) {
        auto k1 = dt * sin(x);
        auto x1 = x + 0.5 * k1;

        auto k2 = dt * sin(x1);
        auto x2 = x + 0.5 * k2;

        auto k3 = dt * sin(x2);
        auto x3 = x + k3;

        auto k4 = dt * sin(x3);

        x += (k1 + 2 * k2 + 2 * k3 + k4) / 6;
    };

    std::vector<double> x = random_vector<double>(n);
    vex::vector<double> X(ctx, x);

    for(int i = 0; i < 100; i++) rk4(X, dt);

    check_sample(X, [&](size_t idx, double a) {
            double s = x[idx];
            for(int i = 0; i < 100; i++)
                runge_kutta_4(sys_func<double>, s, dt);

            BOOST_CHECK_CLOSE(a, s, 1e-8);
            });
}

BOOST_AUTO_TEST_SUITE_END()