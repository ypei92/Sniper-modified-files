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
    if(argc != 1)
    {
        cout<<"usage: ./a.out"<<endl;
        return 1;
    }
    

    string filename1, filename2, filename0, filename3, infilename_pc, outfile_name1;
    Uint64 cycles[4];
    ifstream INFILE0;
    ifstream INFILE1;
    ifstream INFILE2;
    ifstream INFILE3;
    ifstream INFILE_PC;
    ofstream OUTFILE1;
    string string1,string2;
    Uint64 PC, isb, bo;
    int count[4] = {0, 0, 0, 0};
    int nline = 0;
        map<Uint64, int> pc_v;        

        infilename_pc = "pc_choice.csv";
    	INFILE_PC.open(infilename_pc.c_str());
    	if(INFILE_PC.fail()) 
    	{
        	cout<<"fail to open infile_pc:"<< infilename_pc<<endl;
		return 1;
    	}
        while(!INFILE_PC.eof())
    	{
            	INFILE_PC>>PC;
                INFILE_PC>>isb;
                INFILE_PC>>bo;
                nline++;
                count[isb*2 + bo]++;
                
      	}
    	INFILE_PC.close();
        cout<<count[0] << "\t" << count[1] << "\t" << count[2] << "\t" << count[3] << endl;

       return 0;
}
