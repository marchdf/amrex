#include <AMReX_BLBackTrace.H>
#include <AMReX_ParallelDescriptor.H>
#include <AMReX_Print.H>
#include <AMReX_VisMF.H>
#include <AMReX_AsyncOut.H>
#include <AMReX.H>
#include <AMReX_Utility.H>
#ifdef AMREX_USE_MPI
#include <AMReX_MPMD.H>
#endif

#ifdef AMREX_TINY_PROFILING
#include <AMReX_TinyProfiler.H>
#endif

#include <iostream>
#include <sstream>
#include <fstream>
#include <cstring>
#include <cstdio>
#include <csignal>

#if !(defined(_MSC_VER) && defined(__CUDACC__))
//MSVC can't pre-processor cfenv with `Zc:preprocessor`
//https://developercommunity.visualstudio.com/content/problem/1271183/zcpreprocessor-e-crashes-when-given.html
#include <cfenv>
#endif

#if defined(AMREX_EXPORT_DYNAMIC) && defined(__APPLE__)
#include <cxxabi.h>
#include <dlfcn.h>
#define AMREX_BACKTRACE_SUPPORTED 1
#elif defined(__linux__)
#define AMREX_BACKTRACE_SUPPORTED 1
#endif

#ifndef _WIN32
#include <execinfo.h>
#include <unistd.h>
#endif

namespace amrex {

std::stack<std::pair<std::string, std::string> >  BLBackTrace::bt_stack;

void
BLBackTrace::handler(int s)
{

    std::signal(s, SIG_DFL);

    AsyncOut::Finalize();

    switch (s) {
    case SIGSEGV:
        amrex::ErrorStream() << "Segfault\n";
        break;
    case SIGFPE:
        amrex::ErrorStream() << "Erroneous arithmetic operation\n";
        break;
    case SIGILL:
        amrex::ErrorStream() << "SIGILL Invalid, privileged, or ill-formed instruction\n";
        break;
    case SIGTERM:
        amrex::ErrorStream() << "SIGTERM\n";
        break;
    case SIGINT:
        amrex::ErrorStream() << "SIGINT\n";
        break;
    case SIGABRT:
        amrex::ErrorStream() << "SIGABRT\n";
        break;
    }

#if defined(AMREX_BACKTRACE_SUPPORTED) || defined(AMREX_TINY_PROFILING)

    std::string errfilename;
    {
        std::ostringstream ss;
#ifdef AMREX_USE_MPI
        if (MPMD::Initialized()) {
            ss << "Backtrace.prog" << MPMD::MyProgId() << ".";
        } else
#endif
        {
            ss << "Backtrace.";
        }
        ss << ParallelDescriptor::MyProc();
#ifdef AMREX_USE_OMP
        ss << "." << omp_get_thread_num();
#endif
        errfilename = ss.str();
    }

    if (FILE* p = fopen(errfilename.c_str(), "w")) {
#if defined(AMREX_BACKTRACE_SUPPORTED)
        BLBackTrace::print_backtrace_info(p);
#endif
        fclose(p);
    }

    amrex::ErrorStream() << "See " << errfilename << " file for details" << std::endl;

    if (!bt_stack.empty()) {
        std::ofstream errfile;
        errfile.open(errfilename.c_str(), std::ofstream::out | std::ofstream::app);
        if (errfile.is_open()) {
            errfile << std::endl;
            while (!bt_stack.empty()) {
                errfile << "== BACKTRACE == " << bt_stack.top().first
                        <<", " << bt_stack.top().second << "\n";
                bt_stack.pop();
            }
            errfile << std::endl;
        }
    }

#ifdef AMREX_TINY_PROFILING
    {
        std::ofstream errfile;
        errfile.open(errfilename.c_str(), std::ofstream::out | std::ofstream::app);
        if (errfile.is_open()) {
            errfile << std::endl;
            TinyProfiler::PrintCallStack(errfile);
            errfile << std::endl;
        }
    }
#endif

    if (ParallelDescriptor::NProcs() > 1) {
        amrex::Sleep(3);
    }

#endif

    ParallelDescriptor::Abort(s, false);
}

void
BLBackTrace::print_backtrace_info (const std::string& filename)
{
    if (FILE* p = fopen(filename.c_str(), "w"))
    {
      BLBackTrace::print_backtrace_info(p);
      fclose(p);
    }
    else
    {
        amrex::Print() << "Warning @ BLBackTrace::print_backtrace_info: "
                       << filename << " is not a valid output file."
                       << std::endl;
    }
}

#ifdef AMREX_BACKTRACE_SUPPORTED
namespace {
    std::string run_command (std::string const& cmd)
    {
        std::string r;
        if (FILE * ps = popen(cmd.c_str(), "r")) {
            char print_buff[512];
            while (fgets(print_buff, sizeof(print_buff), ps)) {
                r += print_buff;
            }
            pclose(ps);
        }
        return r;
    }

    int file_exists (const char* file)
    {
        int r = 0;
        if (FILE *fp = fopen(file, "r")) {
            fclose(fp);
            r = 1;
        }
        return r;
    }
}
#endif

void
BLBackTrace::print_backtrace_info (FILE* f)
{
#ifdef AMREX_BACKTRACE_SUPPORTED

    const int nbuf = 64;
    void *bt_buffer[nbuf];
    int nentries = backtrace(bt_buffer, nbuf);

#ifdef __linux__

    {
        constexpr std::size_t len = 64;
        char host_name[len];
        host_name[len-1] = '\n';
        // The returned buffer may not include '\n', when truncation occurs.
        // So we insert one just in case.
        const int ret = gethostname(host_name, len-1);
        if (ret == 0) {
            std::fprintf(f, "Host Name: %s\n", host_name);
        }
    }

    char **strings = backtrace_symbols(bt_buffer, nentries);
    if (strings != nullptr) {
        int have_eu_addr2line = 0;
        int have_addr2line = 0;
        std::string eu_cmd;
        {
            have_eu_addr2line = file_exists("/usr/bin/eu-addr2line");
            if (have_eu_addr2line) {
                const pid_t pid = getpid();
                // cmd = "/usr/bin/eu-addr2line -C -f -i --pretty-print -p "
                eu_cmd = "/usr/bin/eu-addr2line -C -f -i -p "
                    + std::to_string(pid);
            }
        }
        std::string cmd;
        {
            have_addr2line = file_exists("/usr/bin/addr2line");
            if (have_addr2line) {
                cmd = "/usr/bin/addr2line -Cpfie " + amrex::system::exename;
            }
        }

        std::fprintf(f, "=== If no file names and line numbers are shown below, one can run\n");
        std::fprintf(f, "            addr2line -Cpfie my_exefile my_line_address\n");
        std::fprintf(f, "    to convert `my_line_address` (e.g., 0x4a6b) into file name and line number.\n");
        std::fprintf(f, "    Or one can use amrex/Tools/Backtrace/parse_bt.py.\n\n");

        std::fprintf(f, "=== Please note that the line number reported by addr2line may not be accurate.\n");
        std::fprintf(f, "    One can use\n");
        std::fprintf(f, "            readelf -wl my_exefile | grep my_line_address'\n");
        std::fprintf(f, "    to find out the offset for that line.\n\n");

        for (int i = 0; i < nentries; ++i)
        {
            std::fprintf(f, "%2d: %s\n", i, strings[i]);

#if !defined(AMREX_USE_OMP) || !defined(__INTEL_COMPILER)
            const bool stack_ptr_not_null = (bt_buffer[i] != nullptr);

            std::string addr2line_result;
            bool try_addr2line = false;
            if (amrex::system::call_addr2line && have_eu_addr2line) {
                if (stack_ptr_not_null) {
                    char print_buff[32];
                    std::snprintf(print_buff,sizeof(print_buff),"%p",bt_buffer[i]);
                    const std::string full_cmd = eu_cmd + " " + print_buff;
                    addr2line_result = run_command(full_cmd);
                    if (addr2line_result.find("??:") != std::string::npos) { // found ??:
                        try_addr2line = true;
                    }
                }
            } else {
                try_addr2line = true;
            }
            if (try_addr2line && amrex::system::call_addr2line && have_addr2line &&
                !amrex::system::exename.empty() && stack_ptr_not_null)
            {
                addr2line_result.clear();
                const std::string line = strings[i];
                std::size_t found_libc = line.find("libc.so");
                if (found_libc == std::string::npos) {
                    std::string addr;
                    {
                        std::size_t found1 = line.rfind('(');
                        std::size_t found2 = line.rfind(')');
                        std::size_t found3 = line.rfind('+');
                        if (found1 != std::string::npos && found2 != std::string::npos && found3 != std::string::npos) {
                            if (found1 < found3 && found3 < found2) {
                                addr = line.substr(found3+1, found2-found3-1);
                            }
                        }
                    }
                    if (!addr.empty()) {
                        std::string full_cmd = cmd;
                        full_cmd.append(" ").append(addr);
                        addr2line_result = run_command(full_cmd);
                        if (addr2line_result.find('?') != std::string::npos) {
                            addr2line_result.clear();
                        }
                    }
                    if (addr2line_result.empty()) {
                        char print_buff[32];
                        std::snprintf(print_buff,sizeof(print_buff),"%p",bt_buffer[i]);
                        std::string full_cmd = cmd;
                        full_cmd.append(" ").append(print_buff);
                        addr2line_result = run_command(full_cmd);
                    }
                }
            }

            if (!addr2line_result.empty()) {
                std::fprintf(f, "    %s", addr2line_result.c_str());
            }
#endif
            std::fprintf(f, "\n");
        }
        std::free(strings);
    }

#elif defined(AMREX_BACKTRACE_SUPPORTED) && defined(__APPLE__)

    const pid_t pid = getpid();
    const std::string cmd = "/usr/bin/atos -p " + std::to_string(pid);
    const int have_atos = file_exists("/usr/bin/atos");

    for (int i = 0; i < nentries; ++i) {
        Dl_info info;
        const bool stack_ptr_not_null = (bt_buffer[i] != nullptr);
        if (dladdr(bt_buffer[i], &info))
        {
            std::string line;
            if (amrex::system::call_addr2line && have_atos && stack_ptr_not_null) {
                char print_buff[32];
                std::snprintf(print_buff,sizeof(print_buff),"%p",bt_buffer[i]);
                const std::string full_cmd = cmd + " " + print_buff;
                line = run_command(full_cmd);
            }
            if (line.empty()) {
                int status;
                char * demangled_name = abi::__cxa_demangle(info.dli_sname, nullptr, 0, &status);
                if (status == 0) {
                    line += demangled_name;
                } else {
                    line += info.dli_fname;
                }
                line += '\n';
                std::free(demangled_name);
            }
            std::fprintf(f, "%2d: %s\n", i, line.c_str());
        }
    }

#endif

#else
    amrex::ignore_unused(f);
#endif
}

BLBTer::BLBTer(const std::string& s, const char* file, int line)
{
    std::ostringstream ss;
    ss << "Line " << line << ", File " << file;
    line_file = ss.str();

#ifdef AMREX_USE_OMP
    if (omp_in_parallel()) {
        std::ostringstream ss0;
        ss0 << "Proc. " << ParallelDescriptor::MyProc()
            << ", Thread " << omp_get_thread_num()
            << ": \"" << s << "\"";
        BLBackTrace::bt_stack.push(std::make_pair(ss0.str(), line_file));
    }
    else {
        #pragma omp parallel
        {
            std::ostringstream ss0;
            ss0 << "Proc. " << ParallelDescriptor::MyProc()
                << ", Master Thread"
                << ": \"" << s << "\"";
            BLBackTrace::bt_stack.push(std::make_pair(ss0.str(), line_file));
        }
    }
#else
    std::ostringstream ss0;
    ss0 << "Proc. " << ParallelDescriptor::MyProc()
        << ": \"" << s << "\"";
    BLBackTrace::bt_stack.push(std::make_pair(ss0.str(), line_file));
#endif
}

BLBTer::~BLBTer()
{
#ifdef AMREX_USE_OMP
    if (omp_in_parallel()) {
        pop_bt_stack();
    }
    else {
        #pragma omp parallel
        {
            pop_bt_stack();
        }
    }
#else
    pop_bt_stack();
#endif
}

void
BLBTer::pop_bt_stack()
{
    if (!BLBackTrace::bt_stack.empty()) {
        if (BLBackTrace::bt_stack.top().second == line_file) {
            BLBackTrace::bt_stack.pop();
        }
    }
}

}
