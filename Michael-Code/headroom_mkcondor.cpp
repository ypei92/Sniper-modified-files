#include <iostream>
#include <fstream>
#include <string>
#include <stdint.h>
#include <stdlib.h>
#include <map>
#include <string>
#include <vector>

using namespace std;
typedef uint64_t Uint64;
typedef struct result
{
    result(){
        isb_hits = 0;
        isb_prefetches = 0;
        bo_hits = 0;
        bo_prefetches = 0;
        hybrid_hits = 0;
        hybrid_prefetches = 0;
    }
    Uint64 isb_hits;
    Uint64 isb_prefetches;
    Uint64 bo_hits;
    Uint64 bo_prefetches;
    Uint64 hybrid_hits;
    Uint64 hybrid_prefetches;
}Result;

int main(int argc, char** argv)
{
    if(argc != 2)
    {
        cout<<"usage: ./a.out [pwd]"<<endl;
        return 1;
    }
    string condor_template = 
	"+Group=\"GRAD\"\n"
	"+Project=\"ARCHITECTURE\"\n"
	"+ProjectDescription=\"Architectural Simulation to study caches using sniper simulator\"\n"
	"universe=vanilla\n"
	"coresize=0\n"
	"getenv=true\n"
	"Rank=Memory\n"
	"notification=Error\n"
	"input=\n";
    string pwd(argv[1]);
    string output_s = "output="+ pwd + "/CONDOR.OUT\n";
    string error_s = "error="+ pwd + "/CONDOR.ERR\n";
    string log_s = "Log=" + pwd + "/CONDOR.LOG\n";
    string req_s="requirements = InMastodon\n";
    string init_s = "initialdir = " + pwd + "\n";
    string exec_s = "executable = " + pwd + "/run.sh\n";
    string q_s = "queue\n";


    string pinballs_s;


    string filename1, filename2, outfile_name1, outfile_name2;
    ifstream INFILE1;
    ifstream INFILE2;
    ofstream OUTFILE1;
    ofstream OUTFILE2;
    string string1,string2;
    
        map<Uint64, int> pc_v;        

        filename1 = "../../run.sh";
    	INFILE1.open(filename1.c_str());
    	if(INFILE1.fail()) 
    	{
        	cout<<"fail to open file1:"<< filename1<<endl;
		return 1;
    	}
        for(int i = 0; i < 10; i++)
    	{
            	INFILE1>>string1;
      	}
        //cout<< string1 <<endl;
        pinballs_s = string1;
    	INFILE1.seekg(0, ios::beg);
    	INFILE1.close();
        string run_template = "#!/bin/bash\n"
        "/u/hejy/CompArch/sniper/run-sniper -c /u/hejy/CompArch/sniper/run_config/hybrid_6_4.cfg -s stop-by-icount:250000000 -d " + pwd + " --pinballs " + pinballs_s + "\n";

    outfile_name1 = "run.condor";
    outfile_name2 = "run.sh";
    OUTFILE1.open(outfile_name1.c_str());
    OUTFILE2.open(outfile_name2.c_str());
    if(OUTFILE1.fail() || OUTFILE2.fail())
    {
        cout<<"fail to open outfile"<<endl;
        return 1;
    }
    OUTFILE1 << condor_template << output_s << error_s << log_s << req_s << init_s << exec_s << q_s << endl;
    OUTFILE2 << run_template << endl;
    OUTFILE1.close();
    OUTFILE2.close();
    return 0;
}
