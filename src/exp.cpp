
#include <cmath>
#include <iostream>

#include "gwnum.h"
#include "cpuid.h"
#include "exp.h"
#include "exception.h"

using namespace arithmetic;

void BaseExp::init(InputNum* input, GWState* gwstate, File* file, TaskState* state, Logging* logging, int iterations)
{
    Task::init(gwstate, file, state, logging, iterations);
    _input = input;
    _timer = getHighResTimer();
    _transforms = -(int)gwstate->handle.fft_count;
    _error_check = _error_check_near ? gwnear_fft_limit(gwstate->gwdata(), 1) == TRUE : _error_check_forced;
}

void BaseExp::done()
{
    _timer = (getHighResTimer() - _timer)/getHighResTimerFrequency();
    _transforms += (int)_gwstate->handle.fft_count;
    _logging->progress().update(1, (int)_gwstate->handle.fft_count/2);
    _logging->set_prefix("");
}

void BaseExp::reinit_gwstate()
{
    double fft_count = _gwstate->handle.fft_count;
    _gwstate->done();
    _input->setup(*_gwstate);
    _gwstate->handle.fft_count = fft_count;
    std::string prefix = _logging->prefix();
    _logging->set_prefix("");
    _logging->error("Restarting using %s\n", _gwstate->fft_description.data());
    _logging->set_prefix(prefix);
    _logging->report_param("fft_desc", _gwstate->fft_description);
    _logging->report_param("fft_len", _gwstate->fft_length);
    _error_check = _error_check_near ? gwnear_fft_limit(_gwstate->gwdata(), 1) == TRUE : _error_check_forced;
}

void FastExp::init(InputNum* input, GWState* gwstate, File* file, Logging* logging, uint32_t x0)
{
    GWASSERT(x0 <= gwstate->maxmulbyconst);
    BaseExp::init(input, gwstate, file, read_state<State>(file), logging, _exp.bitlen() - 1);
    _state_update_period = MULS_PER_STATE_UPDATE;
    _logging->set_prefix(input->display_text() + " ");
    if (state() != nullptr)
        _logging->info("restarting at %.1f%%.\n", 100.0*state()->iteration()/iterations());
    if (_error_check)
        _logging->info("max roundoff check enabled.\n");
    _x0 = x0;
}

void FastExp::execute()
{
    int i, len;

    GWNum X(gw());
    if (state() == nullptr)
    {
        i = 0;
        X = _x0;
        gwset_carefully_count(gw().gwdata(), 30);
    }
    else
    {
        i = state()->iteration();
        X = state()->X();
    }
    gw().setmulbyconst(_x0);
    len = iterations();
    for (; i < len; i++, commit_execute<State>(i, X))
        gw().square(X, X, (_exp.bit(len - i - 1) ? GWMUL_MULBYCONST : 0) | GWMUL_STARTNEXTFFT_IF(!is_last(i)));

    done();
}

void SlowExp::init(InputNum* input, GWState* gwstate, File* file, Logging* logging, const arithmetic::Giant& X0)
{
    BaseExp::init(input, gwstate, file, read_state<State>(file), logging, _exp.bitlen() - 1);
    _state_update_period = MULS_PER_STATE_UPDATE/1.5;
    _logging->set_prefix(input->display_text() + " ");
    if (state() != nullptr)
        _logging->info("restarting at %.1f%%.\n", 100.0*state()->iteration()/iterations());
    _X0 = X0;
}

void SlowExp::execute()
{
    int i, len;

    GWNum X(gw());
    GWNum X0(gw());
    X0 = _X0;
    if (state() == nullptr)
    {
        i = 0;
        X = X0;
        gwset_carefully_count(gw().gwdata(), 30);
    }
    else
    {
        i = state()->iteration();
        X = state()->X();
    }
    len = iterations();
    for (; i < len; i++, commit_execute<State>(i, X))
    {
        gw().square(X, X, GWMUL_STARTNEXTFFT_IF(!is_last(i) || _exp.bit(len - i - 1)));
        if (_exp.bit(len - i - 1))
            gw().mul(X, X0, X, GWMUL_STARTNEXTFFT_IF(!is_last(i)));
    }

    done();
}

void MultipointExp::init(InputNum* input, GWState* gwstate, File* file, Logging* logging)
{
    BaseExp::init(input, gwstate, file, nullptr, logging, _points.back());
    _state_update_period = MULS_PER_STATE_UPDATE;
    State* state = read_state<State>(file);
    if (state != nullptr)
        init_state(state);
}

void MultipointExp::init_state(State* state)
{
    _state.reset(state);
    _logging->progress().update(0, (int)_gwstate->handle.fft_count/2);
    _logging->set_prefix(_input->display_text() + " ");
    if (_state->iteration() > 0)
        _logging->info("restarting at %.1f%%.\n", 100.0*_state->iteration()/iterations());
    if (_error_check)
        _logging->info("max roundoff check enabled.\n");
}

void MultipointExp::release()
{
    _X.reset();
    _U.clear();
}

void MultipointExp::execute()
{
    int i, next_point;
    Giant exp;
    int last_power = -1;

    _X.reset(new GWNum(gw()));
    GWASSERT(state() != nullptr);
    i = state()->iteration();
    X() = state()->X();
    for (next_point = 0; next_point < _points.size() && i >= _points[next_point]; next_point++);
    if (i < 30)
        gwset_carefully_count(gw().gwdata(), 30 - i);

    for (; next_point < _points.size(); next_point++)
    {
        if (_b == 2)
        {
            for (; i < _points[next_point]; i++, commit_execute<State>(i, X()))
                gw().square(X(), X(), GWMUL_STARTNEXTFFT_IF(!is_last(i) && i + 1 != _points[next_point]));
        }
        else
        {
            if (last_power != _points[next_point] - i)
            {
                last_power = _points[next_point] - i;
                exp = _b;
                exp.power(last_power);
            }
            sliding_window(exp);
            i = _points[next_point];
        }

        if (state() == nullptr || state()->iteration() != i)
        {
            check();
            set_state<State>(i, X());
        }
        if (_on_point != nullptr)
        {
            _on_point(i);
            _last_write = std::chrono::system_clock::now();
        }
    }

    done();
}

void MultipointExp::sliding_window(const arithmetic::Giant& exp)
{
    int i, j;
    int len = exp.bitlen() - 1;
    int W;
    for (W = 2; (W < _W || _W == -1) && ((1 << (W + 1)) <= _max_size || _max_size == -1) && (1 << (W - 1)) + len*(1 + 1/(W + 1.0)) > (1 << (W - 0)) + len*(1 + 1/(W + 2.0)); W++);

    _U.reserve((size_t)1 << (W - 1));
    if (_U.size() <= 0)
        _U.emplace_back(gw());
    swap(_U[0], X());
    gw().square(_U[0], X(), GWMUL_STARTNEXTFFT);
    for (i = 1; i < (1 << (W - 1)); i++)
    {
        if (_U.size() <= i)
            _U.emplace_back(gw());
        gw().mul(X(), _U[i - 1], _U[i], GWMUL_FFT_S1 | GWMUL_FFT_S2 | GWMUL_STARTNEXTFFT);
    }

    i = len;
    while (i >= 0)
    {
        if (exp.bit(i) == 0)
        {
            gw().square(X(), X(), GWMUL_STARTNEXTFFT_IF(i > 0));
            i--;
        }
        else
        {
            j = i - W + 1;
            if (j < 0)
                j = 0;
            for (; exp.bit(j) == 0; j++);
            int ui = 0;
            if (i == len)
            {
                while (i >= j)
                {
                    ui <<= 1;
                    ui += exp.bit(i) ? 1 : 0;
                    i--;
                }
                X() = _U[ui/2];
                continue;
            }

            while (i >= j)
            {
                gw().square(X(), X(), GWMUL_STARTNEXTFFT);
                ui <<= 1;
                ui += exp.bit(i) ? 1 : 0;
                i--;
            }

            gw().mul(_U[ui/2], X(), X(), GWMUL_FFT_S1 | GWMUL_STARTNEXTFFT_IF(i > 0));
        }
    }
}

int GerbiczCheckMultipointExp::CHECKS_PER_POINT = 1;

void GerbiczCheckMultipointExp::Gerbicz_params(int iters, double log2b, int& L, int &L2)
{
    int i;
    //if (log2b > 1.5)
    //    log2b /= 2;
    log2b = 1;
    L = (int)sqrt(iters/log2b);
    L2 = iters - iters%L;
    for (i = L + 1; i*i < 2*iters/log2b; i++)
        if (L2 < iters - iters%i)
        {
            L = i;
            L2 = iters - iters%i;
        }
}

double GerbiczCheckMultipointExp::cost()
{
    int n = _points[_points.size() - 1];
    if (_b == 2)
        return n + n/_L + n/_L2*_L;
    else
    {
        double log2b = log2(_b);
        int W;
        for (W = 2; (W < _W || _W == -1) && ((1 << (W + 1)) <= _max_size || _max_size == -1) && (1 << (W - 1)) + log2b*_L*(1 + 1/(W + 1.0)) >(1 << (W - 0)) + log2b*_L*(1 + 1/(W + 2.0)); W++);
        return n/_L + (n/_L + n/_L2)*((1 << (W - 1)) + log2b*_L*(1 + 1/(W + 1.0)));
    }
}

void GerbiczCheckMultipointExp::init(InputNum* input, GWState* gwstate, File* file, File* file_recovery, Logging* logging)
{
    BaseExp::init(input, gwstate, file, read_state<GerbiczCheckState>(file), logging, _points.back());
    _state_update_period = MULS_PER_STATE_UPDATE/log2(_b);
    _file_recovery = file_recovery;
    State* state_recovery = read_state<State>(file_recovery);
    if (state_recovery != nullptr)
        init_state(state_recovery);
}

void GerbiczCheckMultipointExp::init_state(State* state)
{
    _logging->progress().update(0, (int)_gwstate->handle.fft_count/2);
    _logging->set_prefix(_input->display_text() + " ");
    if (!_state_recovery)
    {
        _logging->info("Gerbicz check enabled, L2 = %d*%d.\n", _L, _L2/_L);
        if (_error_check)
            _logging->info("max roundoff check enabled.\n");
    }
    _state_recovery.reset(state);
    if (!_state || _state->iteration() < _state_recovery->iteration() || _state->iteration() >= _state_recovery->iteration() + _L2)
    {
        _state.reset(new TaskState(0));
        _state->set(_state_recovery->iteration());
    }
    if (_state->iteration() > 0)
        _logging->info("restarting at %.1f%%.\n", 100.0*_state->iteration()/iterations());
}

void GerbiczCheckMultipointExp::write_state()
{
    if (_file_recovery != nullptr && _state_recovery && !_state_recovery->is_written())
        _file_recovery->write(*_state_recovery);
    Task::write_state();
}

void GerbiczCheckMultipointExp::release()
{
    _recovery_op = 0;
    _R.reset();
    _D.reset();
    MultipointExp::release();
}

void GerbiczCheckMultipointExp::setup()
{
    if (!_R)
    {
        _R.reset(new GWNum(gw()));
        GWASSERT(state() != nullptr);
        R() = state()->X();
    }
}

void GerbiczCheckMultipointExp::execute()
{
    int i, j, next_point;
    Giant exp;
    int last_power = -1;

    _X.reset(new GWNum(gw()));
    _D.reset(new GWNum(gw()));
    if (state_check() == nullptr)
    {
        i = state()->iteration();
        X() = R();
        D() = R();
    }
    else
    {
        i = state_check()->iteration();
        X() = state_check()->X();
        D() = state_check()->D();
    }
    for (next_point = 0; next_point < _points.size() && i >= _points[next_point]; next_point++);
    if (i < 30)
        gwset_carefully_count(gw().gwdata(), 30 - i);

    for (; next_point < _points.size(); next_point++)
    {
        int L = _L;
        int L2 = _L2;
        while ((_points[next_point] - state()->iteration()) < L2 && L > 1)
        {
            L /= 2;
            L2 = L*L;
            last_power = -1;
        }
        GWASSERT(i - state()->iteration() <= L2);

        if (_b == 2)
        {
            for (j = i - state()->iteration(); j < L2; j++, i++, commit_execute<GerbiczCheckState>(i, X(), D()))
            {
                gw().square(X(), X(), GWMUL_STARTNEXTFFT_IF(!is_last(i) && i + 1 != _points[next_point] && j + 1 != L2));
                if (j + 1 != L2 && i + 1 == _points[next_point])
                {
                    check();
                    set_state<GerbiczCheckState>(i + 1, X(), D());
                    if (_on_point != nullptr)
                        _on_point(i + 1);
                    next_point++;
                }
                if (j + 1 != L2 && (j + 1)%L == 0)
                    gw().mul(X(), D(), D(), GWMUL_FFT_S1 | GWMUL_STARTNEXTFFT_IF(j + 1 + L != L2));
            }
        }
        else
        {
            GWASSERT((i - state()->iteration())%L == 0);
            for (j = i - state()->iteration(); j < L2; j += L, i += L, commit_execute<GerbiczCheckState>(i, X(), D()))
            {
                if (last_power != L)
                {
                    last_power = L;
                    exp = _b;
                    exp.power(last_power);
                }
                sliding_window(exp);
                if (j + L != L2 && i + L == _points[next_point])
                {
                    check();
                    set_state<GerbiczCheckState>(i + L, X(), D());
                    if (_on_point != nullptr)
                        _on_point(i + L);
                    next_point++;
                }
                if (j + L != L2)
                    gw().mul(X(), D(), D(), GWMUL_FFT_S1 | GWMUL_STARTNEXTFFT_IF(j + L + L != L2));
            }
        }
        check();

        _logging->debug("performing Gerbicz check at %d.\n", i);
        GWNum T(D());
        gw().carefully().mul(X(), D(), D(), 0);
        swap(T, X());
        if (_b == 2)
        {
            for (j = 0; j < L; j++)
                gw().carefully().square(X(), X(), 0);
        }
        else
        {
            GWArithmetic* tmpgw = _gw;
            _gw = &gw().carefully();
            if (last_power != L)
            {
                last_power = L;
                exp = _b;
                exp.power(last_power);
            }
            sliding_window(exp);
            _gw = tmpgw;
        }
        gw().carefully().mul(R(), X(), X(), 0);
        gw().carefully().sub(X(), D(), X(), 0);
        swap(T, X());
        if (T != 0 || D() == 0)
        {
            _logging->error("Gerbicz check failed at %.1f%%.\n", 100.0*i/iterations());
            _state.reset(new TaskState(0));
            _state->set(_state_recovery->iteration());
            _restart_op = _recovery_op;
            throw TaskRestartException();
        }
        else
        {
            R() = X();
            D() = X();
            Giant tmp;
            tmp = R();
            _state_recovery.reset(new State(i, tmp));
            _state.reset(new TaskState(0));
            _state->set(i);
            on_state();
            _recovery_op = _restart_op;
            _restart_count = 0;
            if (i != _points[next_point])
            {
                next_point--;
                continue;
            }
        }

        if (_on_point != nullptr)
        {
            _on_point(i);
            _last_write = std::chrono::system_clock::now();
        }
    }

    done();
}