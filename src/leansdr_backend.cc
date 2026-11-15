#include "leansdr_backend.h"
#include <vector>
#include <string>
#include <cstring>
#include <cstdio>
#include <unistd.h>
#include <limits.h>
#include <sys/prctl.h>

int leandvb_main(int argc,const char **argv);
extern "C" int leansdr_backend_main(int argc,char **argv) {
    prctl(PR_SET_NAME,"leansdr-dvb",0,0,0);
    std::vector<const char *> args;
    std::string helper;
    for (int i=0;i<argc;++i) {
        // Accept legacy visualizer options without creating X11 windows.
        if (!strcmp(argv[i],"--gui") || !strcmp(argv[i],"--linger")) continue;
        args.push_back(argv[i]);
        if (!strcmp(argv[i],"--ldpc-helper") && i+1<argc &&
            (!strcmp(argv[i+1],"builtin") || !strcmp(argv[i+1],"builtin-gpu"))) {
            const bool gpu = !strcmp(argv[i+1],"builtin-gpu");
            char path[PATH_MAX]; ssize_t n=readlink("/proc/self/exe",path,sizeof(path)-1);
            if (n<=0) return 2;
            path[n]=0; char *slash=strrchr(path,'/'); if (!slash) return 2; *slash=0;
            helper=std::string(path)+"/ldpc_tool"
#ifdef _WIN32
                ".exe"
#endif
                ;
            if (gpu) {
#ifndef _WIN32
                const std::string candidate = std::string(path)+"/ldpc_tool_gpu";
                if (!access(candidate.c_str(),X_OK)) helper = candidate;
                else fprintf(stderr,"DVB-S2 LDPC: GPU helper missing; using production CPU\n");
#else
                fprintf(stderr,"DVB-S2 LDPC: GPU backend not validated on Windows; using production CPU\n");
#endif
            }
            if (access(helper.c_str(),X_OK)) { fprintf(stderr,"Missing bundled LDPC helper: %s\n",helper.c_str()); return 2; }
            args.push_back(helper.c_str()); ++i;
        }
    }
    args.push_back(nullptr);
    return leandvb_main(args.size()-1,args.data());
}
