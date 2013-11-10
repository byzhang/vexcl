#ifndef VEXCL_STENCIL_HPP
#define VEXCL_STENCIL_HPP

/*
The MIT License

Copyright (c) 2012-2013 Denis Demidov <ddemidov@ksu.ru>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

/**
 * \file   stencil.hpp
 * \author Denis Demidov <ddemidov@ksu.ru>
 * \brief  Stencil convolution.
 */

#include <vector>
#include <map>
#include <sstream>
#include <cassert>
#include <vexcl/vector.hpp>

namespace vex {

/// \cond INTERNAL

template <class S, class V>
struct conv
    : vector_expression< boost::proto::terminal< additive_vector_transform >::type >
{
    typedef typename S::value_type value_type;

    const S &s;
    const V &x;

    value_type scale;

    conv(const S &s, const V &x) : s(s), x(x), scale(1) {}

    template<bool negate, bool append>
    void apply(vector<value_type> &y) const
    {
        s.convolve(x, y, append ? 1 : 0, negate ? -scale : scale);
    }
};

namespace traits {

template <typename S, class V>
struct is_scalable< conv<S, V> > : std::true_type {};

} // namespace traits

#ifdef VEXCL_MULTIVECTOR_HPP

template <class S, class V>
struct multiconv
    : multivector_expression<
        boost::proto::terminal< additive_multivector_transform >::type
      >
{
    typedef typename S::value_type value_type;

    const S &s;
    const V &x;

    value_type scale;

    multiconv(const S &s, const V &x) : s(s), x(x), scale(1) {}

    template <bool negate, bool append>
    void apply(multivector<value_type, traits::number_of_components<V>::value> &y) const
    {
        for(size_t i = 0; i < traits::number_of_components<V>::value; i++)
            s.convolve(x(i), y(i), append ? 1 : 0, negate ? -scale : scale);
    }
};

namespace traits {

template <typename S, class V>
struct is_scalable< multiconv<S, V> > : std::true_type {};

} // namespace traits

#endif

template <typename T>
class stencil_base {
    protected:
        template <class Iterator>
        stencil_base(
                const std::vector<cl::CommandQueue> &queue,
                unsigned width, unsigned center, Iterator begin, Iterator end
                );

        void exchange_halos(const vex::vector<T> &x) const;

        const std::vector<cl::CommandQueue> &queue;

        mutable std::vector<T>  hbuf;
        std::vector<cl::Buffer> dbuf;
        std::vector<cl::Buffer> s;

        int lhalo;
        int rhalo;
};

template <typename T> template <class Iterator>
stencil_base<T>::stencil_base(
        const std::vector<cl::CommandQueue> &queue,
        unsigned width, unsigned center, Iterator begin, Iterator end
        )
    : queue(queue), hbuf(queue.size() * (width - 1)),
      dbuf(queue.size()), s(queue.size()),
      lhalo(center), rhalo(width - center - 1)
{
    assert(queue.size());
    assert(lhalo >= 0);
    assert(rhalo >= 0);
    assert(width);
    assert(center < width);

    for(unsigned d = 0; d < queue.size(); d++) {
        cl::Context context = qctx(queue[d]);
        cl::Device  device  = qdev(queue[d]);

        if (begin != end) {
            s[d] = cl::Buffer(context, CL_MEM_READ_ONLY, (end - begin) * sizeof(T));

            queue[d].enqueueWriteBuffer(s[d], CL_FALSE, 0,
                    (end - begin) * sizeof(T), &begin[0]);
        }

        // Allocate one element more than needed, to be sure size is nonzero.
        dbuf[d] = cl::Buffer(context, CL_MEM_READ_WRITE, width * sizeof(T));
    }

    for(unsigned d = 0; d < queue.size(); d++) queue[d].finish();
}

template <typename T>
void stencil_base<T>::exchange_halos(const vex::vector<T> &x) const {
    int width = lhalo + rhalo;

    if ((queue.size() <= 1) || (width <= 0)) return;

    // Get halos from neighbours.
    for(unsigned d = 0; d < queue.size(); d++) {
        if (!x.part_size(d)) continue;

        // Get halo from left neighbour.
        if (d > 0 && lhalo > 0) {
            size_t end   = x.part_start(d);
            size_t begin = end >= static_cast<unsigned>(lhalo) ?  end - lhalo : 0;
            size_t size  = end - begin;
            x.read_data(begin, size, &hbuf[d * width + lhalo - size], CL_FALSE);
        }

        // Get halo from right neighbour.
        if (d + 1 < queue.size() && rhalo > 0) {
            size_t begin = x.part_start(d + 1);
            size_t end   = std::min(begin + rhalo, x.size());
            size_t size  = end - begin;
            x.read_data(begin, size, &hbuf[d * width + lhalo], CL_FALSE);
        }
    }

    // Wait for the end of transfer.
    for(unsigned d = 0; d < queue.size(); d++) queue[d].finish();

    // Write halos to a local buffer.
    for(unsigned d = 0; d < queue.size(); d++) {
        if (!x.part_size(d)) continue;

        if (d > 0 && lhalo > 0) {
            size_t end   = x.part_start(d);
            size_t begin = end >= static_cast<unsigned>(lhalo) ?  end - lhalo : 0;
            size_t size  = end - begin;
            if (size)
                std::fill(&hbuf[d * width], &hbuf[d * width + lhalo - size], hbuf[d * width + lhalo - size]);
            else
                std::fill(&hbuf[d * width], &hbuf[d * width + lhalo - size], static_cast<T>(x[0]));
        }

        if (d + 1 < queue.size() && rhalo > 0) {
            size_t begin = x.part_start(d + 1);
            size_t end   = std::min(begin + rhalo, x.size());
            size_t size  = end - begin;
            if (size)
                std::fill(&hbuf[d * width + lhalo + size], &hbuf[(d + 1) * width], hbuf[d * width + lhalo + size - 1]);
            else
                std::fill(&hbuf[d * width + lhalo + size], &hbuf[(d + 1) * width], static_cast<T>(x[x.size()-1]));

        }

        if ((d > 0 && lhalo > 0) || (d + 1 < queue.size() && rhalo > 0))
            queue[d].enqueueWriteBuffer(dbuf[d], CL_FALSE, 0, width * sizeof(T),
                    &hbuf[d * width]);
    }

    // Wait for the end of transfer.
    for(unsigned d = 0; d < queue.size(); d++) queue[d].finish();
}

/// \endcond

/// Stencil.
/**
 * Should be used for stencil convolutions with vex::vectors as in
 * \code
 * void convolve(
 *          const vex::stencil<double> &s,
 *          const vex::vector<double>  &x,
 *          vex::vector<double> &y)
 * {
 *     y = x * s;
 * }
 * \endcode
 * Stencil should be small enough to fit into local memory of all compute
 * devices it resides on.
 */
template <typename T>
class stencil : private stencil_base<T> {
    public:
        typedef T value_type;

        /// Costructor.
        /**
         * \param queue  vector of queues. Each queue represents one
         *               compute device.
         * \param st     vector holding stencil values.
         * \param center center of the stencil.
         */
        stencil(const std::vector<cl::CommandQueue> &queue,
                const std::vector<T> &st, unsigned center
                )
            : stencil_base<T>(queue, static_cast<unsigned>(st.size()), center, st.begin(), st.end()),
              conv(queue.size()), loc_s(queue.size()), loc_x(queue.size())
        {
            init(static_cast<unsigned>(st.size()));
        }

        /// Costructor.
        /**
         * \param queue  vector of queues. Each queue represents one
         *               compute device.
         * \param begin  iterator to begin of sequence holding stencil data.
         * \param end    iterator to end of sequence holding stencil data.
         * \param center center of the stencil.
         */
        template <class Iterator>
        stencil(const std::vector<cl::CommandQueue> &queue,
                Iterator begin, Iterator end, unsigned center
                )
            : stencil_base<T>(queue, static_cast<unsigned>(end - begin), center, begin, end),
              conv(queue.size()), loc_s(queue.size()), loc_x(queue.size())
        {
            init(static_cast<unsigned>(end - begin));
        }

#ifndef BOOST_NO_INITIALIZER_LISTS
        /// Costructor.
        /**
         * \param queue  vector of queues. Each queue represents one
         *               compute device.
         * \param list   intializer list holding stencil values.
         * \param center center of the stencil.
         */
        stencil(const std::vector<cl::CommandQueue> &queue,
                std::initializer_list<T> list, unsigned center
                )
            : stencil_base<T>(queue, list.size(), center, list.begin(), list.end()),
              conv(queue.size()), loc_s(queue.size()), loc_x(queue.size())
        {
            init(list.size());
        }
#endif

        /// Convolve stencil with a vector.
        /**
         * y = alpha * y + beta * conv(x);
         * \param x input vector.
         * \param y output vector.
         * \param alpha Scaling coefficient in front of y.
         * \param beta  Scaling coefficient in front of convolution.
         */
        void convolve(const vex::vector<T> &x, vex::vector<T> &y,
                T alpha = 0, T beta = 1) const;
    private:
        typedef stencil_base<T> Base;

        using Base::queue;
        using Base::hbuf;
        using Base::dbuf;
        using Base::s;
        using Base::lhalo;
        using Base::rhalo;

        mutable std::vector<backend::kernel> conv;
        std::vector<cl::LocalSpaceArg>  loc_s;
        std::vector<cl::LocalSpaceArg>  loc_x;

        void init(unsigned width);

        static const detail::kernel_cache_entry& slow_conv(const cl::CommandQueue &queue);
        static const detail::kernel_cache_entry& fast_conv(const cl::CommandQueue &queue);
};

namespace detail {

template <typename T>
inline void define_read_x(backend::source_generator &source) {
    source.function<T>("read_x")
        .open("(")
            .template parameter<ptrdiff_t>("g_id")
            .template parameter<size_t>("n")
            .template parameter<char>("has_left")
            .template parameter<char>("has_right")
            .template parameter<int>("lhalo")
            .template parameter<int>("rhalo")
            .template parameter< global_ptr<const T> >("xloc")
            .template parameter< global_ptr<const T> >("xrem")
        .close(")").open("{");

    source.new_line() << "if (g_id >= 0 && g_id < n)";
    source.open("{");
    source.new_line() << "return xloc[g_id];";
    source.close("}");
    source.new_line() << "else if (g_id < 0)";
    source.open("{");
    source.new_line() << "if (has_left) "
        "return (lhalo + g_id >= 0) ? xrem[lhalo + g_id] : 0;";
    source.new_line() << "else return xloc[0];";
    source.close("}");
    source.new_line() << "else";
    source.open("{");
    source.new_line() << "if (has_right) "
        "return (g_id < n + rhalo) ? xrem[lhalo + g_id - n] : 0;";
    source.new_line() << "else return xloc[n - 1];";
    source.close("}").close("}");
}

}

template <typename T>
const detail::kernel_cache_entry& stencil<T>::slow_conv(const cl::CommandQueue &queue) {
    using namespace detail;

    static kernel_cache cache;

    cl::Context context = qctx(queue);
    cl::Device  device  = qdev(queue);

    auto kernel = cache.find(context());

    if (kernel == cache.end()) {
        backend::source_generator source(queue);

        define_read_x<T>(source);

        source.kernel("slow_conv")
            .open("(")
                .template parameter<size_t>("n")
                .template parameter<char>("has_left")
                .template parameter<char>("has_right")
                .template parameter<int>("lhalo")
                .template parameter<int>("rhalo")
                .template parameter< global_ptr<const T> >("s")
                .template parameter< global_ptr<const T> >("xloc")
                .template parameter< global_ptr<const T> >("xrem")
                .template parameter< global_ptr<T> >("y")
                .template parameter<T>("alpha")
                .template parameter<T>("beta")
                .template parameter< shared_ptr<T> >("loc_s")
                .template parameter< shared_ptr<T> >("loc_x")
            .close(")").open("{");

        source.grid_stride_loop().open("{");

        source.new_line() << type_name<T>() << " sum = 0;";
        source.new_line() << "for(int j = -lhalo; j <= rhalo; j++)";
        source.open("{");
        source.new_line() << "sum += s[lhalo + j] * read_x(("
            << type_name<ptrdiff_t>()
            << ")idx + j, n, has_left, has_right, lhalo, rhalo, xloc, xrem);";
        source.close("}");
        source.new_line() << "if (alpha) y[idx] = alpha * y[idx] + beta * sum;";
        source.new_line() << "else y[idx] = beta * sum;";
        source.close("}").close("}");

        backend::kernel krn(queue, source.str(), "slow_conv");
        kernel = cache.insert(std::make_pair(context(), krn)).first;
    }

    return kernel->second;
}

template <typename T>
const detail::kernel_cache_entry& stencil<T>::fast_conv(const cl::CommandQueue &queue) {
    using namespace detail;

    static kernel_cache cache;

    cl::Context context = qctx(queue);
    cl::Device  device  = qdev(queue);

    auto kernel = cache.find(context());

    if (kernel == cache.end()) {
        backend::source_generator source(queue);

        define_read_x<T>(source);

        source.kernel("fast_conv")
            .open("(")
                .template parameter<size_t>("n")
                .template parameter<char>("has_left")
                .template parameter<char>("has_right")
                .template parameter<int>("lhalo")
                .template parameter<int>("rhalo")
                .template parameter< global_ptr<const T> >("s")
                .template parameter< global_ptr<const T> >("xloc")
                .template parameter< global_ptr<const T> >("xrem")
                .template parameter< global_ptr<T> >("y")
                .template parameter<T>("alpha")
                .template parameter<T>("beta")
                .template parameter< shared_ptr<T> >("S")
                .template parameter< shared_ptr<T> >("X")
            .close(")").open("{");
        source.new_line() << "size_t grid_size = get_global_size(0);";
        source.new_line() << "int l_id = get_local_id(0);";
        source.new_line() << "int block_size = get_local_size(0);";
        source.new_line() << "async_work_group_copy(S, s, lhalo + rhalo + 1, 0);";
        source.new_line() << "for(long g_id = get_global_id(0), pos = 0; pos < n; g_id += grid_size, pos += grid_size)";
        source.open("{");
        source.new_line() << "for(int i = l_id, j = g_id - lhalo; i < block_size + lhalo + rhalo; i += block_size, j += block_size)";
        source.open("{");
        source.new_line() << "X[i] = read_x(j, n, has_left, has_right, lhalo, rhalo, xloc, xrem);";
        source.close("}");
        source.new_line().barrier();
        source.new_line() << "if (g_id < n)";
        source.open("{");
        source.new_line() << type_name<T>() << " sum = 0;";
        source.new_line() << "for(int j = -lhalo; j <= rhalo; j++)";
        source.open("{");
        source.new_line() << "sum += S[lhalo + j] * X[lhalo + l_id + j];";
        source.close("}");
        source.new_line() << "if (alpha) "
            "y[g_id] = alpha * y[g_id] + beta * sum;";
        source.new_line() << "else y[g_id] = beta * sum;";
        source.close("}");
        source.new_line().barrier();
        source.close("}").close("}");

        backend::kernel krn(queue, source.str(), "fast_conv");
                //[width](size_t wgs) { return (wgs + 2 * width - 1) * sizeof(T); });
        kernel = cache.insert(std::make_pair(context(), krn)).first;
    }

    return kernel->second;
}

template <typename T>
void stencil<T>::init(unsigned width) {
    for (unsigned d = 0; d < queue.size(); d++) {
        cl::Device device = qdev(queue[d]);

        // TODO: better estimate.
        if (is_cpu(device) || width > 64) {
            conv[d]  = slow_conv(queue[d]);
            loc_s[d] = vex::Local(1);
            loc_x[d] = vex::Local(1);
        } else {
            conv[d] = fast_conv(queue[d]);
            loc_s[d] = vex::Local(sizeof(T) * width);
            loc_x[d] = vex::Local(sizeof(T) * (conv[d].workgroup_size() + lhalo + rhalo));
        }
    }
}

template <typename T>
void stencil<T>::convolve(const vex::vector<T> &x, vex::vector<T> &y,
        T alpha, T beta
        ) const
{
    Base::exchange_halos(x);

    for(unsigned d = 0; d < queue.size(); d++) {
        if (size_t psize = x.part_size(d)) {
            cl::Context context = qctx(queue[d]);
            cl::Device  device  = qdev(queue[d]);

            char has_left  = d > 0;
            char has_right = d + 1 < queue.size();

            conv[d].push_arg(psize);
            conv[d].push_arg(has_left);
            conv[d].push_arg(has_right);
            conv[d].push_arg(lhalo);
            conv[d].push_arg(rhalo);
            conv[d].push_arg(s[d]);
            conv[d].push_arg(x(d));
            conv[d].push_arg(dbuf[d]);
            conv[d].push_arg(y(d));
            conv[d].push_arg(alpha);
            conv[d].push_arg(beta);
            conv[d].push_arg(loc_s[d]);
            conv[d].push_arg(loc_x[d]);

            conv[d](queue[d]);
        }
    }
}

template <typename T>
conv< stencil<T>, vector<T> >
operator*( const stencil<T> &s, const vector<T> &x ) {
    return conv< stencil<T>, vector<T> >(s, x);
}

template <typename T>
conv< stencil<T>, vector<T> >
operator*(const vector<T> &x, const stencil<T> &s) {
    return conv< stencil<T>, vector<T> >(s, x);
}

#ifdef VEXCL_MULTIVECTOR_HPP

template <typename T, size_t N>
multiconv< stencil<T>, multivector<T, N> >
operator*( const stencil<T> &s, const multivector<T, N> &x ) {
    return multiconv< stencil<T>, multivector<T, N> >(s, x);
}

template <typename T, size_t N>
multiconv< stencil<T>, multivector<T, N> >
operator*( const multivector<T, N> &x, const stencil<T> &s ) {
    return multiconv< stencil<T>, multivector<T, N> >(s, x);
}

#endif

/// User-defined stencil operator
/**
 * Is used to define custom stencil operator. For example, to implement the
 * following nonlinear operator:
 * \code
 * y[i] = x[i] + pow3(x[i-1] + x[i+1]);
 * \endcode
 * one has to write:
 * \code
 * extern const char pow3_oper_body[] = "return X[0] + pow(X[-1] + X[1], 3);";
 * StencilOperator<double, 3, 1, pow3_oper_body> pow3_oper(ctx);
 *
 * y = pow3_oper(x);
 * \endcode
 */
template <typename T, unsigned width, unsigned center, class Impl>
class StencilOperator : private stencil_base<T> {
    public:
        typedef T value_type;

        StencilOperator(const std::vector<cl::CommandQueue> &queue);

        conv< StencilOperator, vector<T> >
        operator()(const vector<T> &x) const {
            return conv< StencilOperator, vector<T> >(*this, x);
        }

#ifdef VEXCL_MULTIVECTOR_HPP
        template <size_t N>
        multiconv< StencilOperator, multivector<T, N> >
        operator()(const multivector<T, N> &x) const {
            return multiconv< StencilOperator, multivector<T, N> >(*this, x);
        }
#endif

        void convolve(const vex::vector<T> &x, vex::vector<T> &y,
                T alpha = 0, T beta = 1) const;
    private:
        typedef stencil_base<T> Base;

        using Base::queue;
        using Base::hbuf;
        using Base::dbuf;
        using Base::lhalo;
        using Base::rhalo;
};

template <typename T, unsigned width, unsigned center, class Impl>
StencilOperator<T, width, center, Impl>::StencilOperator(
        const std::vector<cl::CommandQueue> &queue)
    : Base(queue, width, center, static_cast<T*>(0), static_cast<T*>(0))
{ }

template <typename T, unsigned width, unsigned center, class Impl>
void StencilOperator<T, width, center, Impl>::convolve(
        const vex::vector<T> &x, vex::vector<T> &y, T alpha, T beta) const
{
    using namespace detail;

    static kernel_cache cache;
    static std::map<cl_context, cl::LocalSpaceArg> lmem;

    Base::exchange_halos(x);

    for(unsigned d = 0; d < queue.size(); d++) {
        cl::Context context = qctx(queue[d]);
        cl::Device  device  = qdev(queue[d]);

        auto kernel = cache.find(context());

        if (kernel == cache.end()) {
            backend::source_generator source(queue[d]);

            define_read_x<T>(source);

            source.function<T>("stencil_oper")
                .open("(")
                    .template parameter< shared_ptr<const T> >("X")
                .close(")").open("{").new_line();
            source << Impl::body();
            source.close("}");

            source.kernel("convolve")
                .open("(")
                    .template parameter<size_t>("n")
                    .template parameter<char>("has_left")
                    .template parameter<char>("has_right")
                    .template parameter<int>("lhalo")
                    .template parameter<int>("rhalo")
                    .template parameter< global_ptr<const T> >("xloc")
                    .template parameter< global_ptr<const T> >("xrem")
                    .template parameter< global_ptr<T> >("y")
                    .template parameter<T>("alpha")
                    .template parameter<T>("beta")
                    .template parameter< shared_ptr<T> >("X")
                .close(")").open("{");

            source.new_line() << "size_t grid_size = get_global_size(0);";
            source.new_line() << "int l_id = get_local_id(0);";
            source.new_line() << "int block_size = get_local_size(0);";
            source.new_line() << "for(long g_id = get_global_id(0), pos = 0; pos < n; g_id += grid_size, pos += grid_size)";
            source.open("{");
            source.new_line() << "for(int i = l_id, j = g_id - lhalo; i < block_size + lhalo + rhalo; i += block_size, j += block_size)";
            source.open("{");
            source.new_line() << "X[i] = read_x(j, n, has_left, has_right, lhalo, rhalo, xloc, xrem);";
            source.close("}");
            source.new_line().barrier();
            source.new_line() << "if (g_id < n)";
            source.open("{");
            source.new_line() << type_name<T>() << " sum = stencil_oper(X + lhalo + l_id);";
            source.new_line() << "if (alpha) y[g_id] = alpha * y[g_id] + beta * sum;";
            source.new_line() << "else y[g_id] = beta * sum;";
            source.close("}");
            source.new_line().barrier();
            source.close("}").close("}");

            backend::kernel krn(queue[d], source.str(), "convolve",
                    [](size_t wgs) { return (width + wgs - 1) * sizeof(T); }
                    );
            kernel = cache.insert(std::make_pair(context(), krn)).first;

            lmem[context()] = vex::Local(sizeof(T) * (krn.workgroup_size() + width - 1));
        }

        if (size_t psize = x.part_size(d)) {
            char has_left  = d > 0;
            char has_right = d + 1 < queue.size();

            kernel->second.push_arg(psize);
            kernel->second.push_arg(has_left);
            kernel->second.push_arg(has_right);
            kernel->second.push_arg(lhalo);
            kernel->second.push_arg(rhalo);
            kernel->second.push_arg(x(d));
            kernel->second.push_arg(dbuf[d]);
            kernel->second.push_arg(y(d));
            kernel->second.push_arg(alpha);
            kernel->second.push_arg(beta);
            kernel->second.push_arg(lmem[context()]);

            kernel->second(queue[d]);
        }
    }
}

/// Macro to declare a user-defined stencil operator type.
/**
 * \code
 * VEX_STENCIL_OPERATOR_TYPE(pow3_oper_t, double, 3, 1, "return X[0] + pow(X[-1] + X[1], 3.0);");
 * pow3_oper_t pow3_oper(ctx);
 * output = pow3_oper(input);
 * \endcode
 *
 * \note Should be used in case same operator is used in several places (to
 * save on OpenCL kernel recompilations). Otherwise VEX_STENCIL_OPERATOR should
 * be used locally.
 */
#define VEX_STENCIL_OPERATOR_TYPE(name, type, width, center, body_str) \
    struct name : vex::StencilOperator<type, width, center, name> { \
        name(const std::vector<cl::CommandQueue> &q) : vex::StencilOperator<type, width, center, name>(q) {} \
        static std::string body() { return body_str; } \
    }

/// Macro to declare a user-defined stencil operator.
/**
 * \code
 * VEX_STENCIL_OPERATOR(pow3_oper, double, 3, 1, "return X[0] + pow(X[-1] + X[1], 3.0);", queue);
 * output = pow3_oper(input);
 * \endcode
 */
#define VEX_STENCIL_OPERATOR(name, type, width, center, body, queue) \
    VEX_STENCIL_OPERATOR_TYPE(stencil_operator_##name##_t, type, width, center, body) name(queue)

} // namespace vex

#endif
