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
    int nline = 0;
        map<Uint64, int> pc_v;        

        infilename_pc = "naive_hybrid/pc_choice.csv";
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
                pc_v[PC] = 3;
      	}
    	INFILE_PC.close();
    

    filename3 = "naive_hybrid/sim.out";
    INFILE3.open(filename3.c_str());
    if(INFILE3.fail()) 
    	{
        	cout<<"fail to open infile3:"<< filename3 <<endl;
		return 1;
    	}
        while(!INFILE3.eof())
    	{
            	INFILE3>>string1;
                if(string1 == "Cycles")
                {
                    INFILE3>>string2;
                    INFILE3>>cycles[3];
                    break;
                }
      	}
    	INFILE3.close();
    
    OUTFILE1.open("pc_choice.csv");
    if(OUTFILE1.fail())
    {
        cout<<"fail to open output pc_choice"<<endl;    
        return 1;
    }
    int i = 0; 
    for(map<Uint64, int>::iterator it=pc_v.begin(); i < nline, it!= pc_v.end(); i++, it++)
    {
        filename0 = "headroom/pc_choice" + to_string(i) + "_0/sim.out";
        filename1 = "headroom/pc_choice" + to_string(i) + "_1/sim.out";
        filename2 = "headroom/pc_choice" + to_string(i) + "_2/sim.out";
	INFILE0.open(filename0.c_str());
	INFILE1.open(filename1.c_str());
	INFILE2.open(filename2.c_str());
        if(INFILE1.fail() || INFILE0.fail() || INFILE2.fail()  ) 
    	{
        	cout<<"fail to open infile012:" <<endl;
		return 1;
    	}
        while(!INFILE0.eof())
    	{
            	INFILE0>>string1;
		INFILE1>>string1;
		INFILE2>>string1;
                if(string1 == "Cycles")
                {
                    INFILE0>>string2;
		    INFILE1>>string2;
                    INFILE2>>string2;
                    
                    INFILE0>>cycles[0];
                    INFILE1>>cycles[1];
                    INFILE2>>cycles[2];
                    break;
                }
      	}
        INFILE0.close();
    	INFILE1.close();
    	INFILE2.close();

        Uint64 min_cycles = cycles[0], min_choice = 0;
        for(int i = 0; i < 4; i++)
        {
            if(cycles[i] < min_cycles)
            {
                min_cycles = cycles[i];
                min_choice = i;
            }
        } 
        //cout<<it->first<< " " << min_choice/2 << " "<< min_choice%2 <<endl;
        OUTFILE1<<it->first<< " " << min_choice/2 << " "<< min_choice%2 <<endl;
        
    }
    OUTFILE1.close();
    return 0;
}
