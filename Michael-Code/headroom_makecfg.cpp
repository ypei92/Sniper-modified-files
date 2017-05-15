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
    if(argc != 3)
    {
        cout<<"usage: ./a.out benchmark [num]"<<endl;
        return 1;
    }
    string filename1, filename2, outfile_name;
    ifstream INFILE1;
    ifstream INFILE2;
    ofstream OUTFILE;
    string string1,string2;
    int line1 = 0, line2 = 0;
    Uint64 PC, hits, prefetches;
    int isb, bo;   
    int temp_count = 0;    
    string benchmark_name(argv[1]);
    
    for(int i = 1; i <= atoi(argv[2]); i++)
    {
        map<Uint64, int> pc_v;        

        filename1 = "./" + benchmark_name+ "/" + to_string(i) +"/pc_choice.csv";
    	INFILE1.open(filename1.c_str());
    	if(INFILE1.fail()) 
    	{
        	cout<<"fail to open file1:"<< filename1<<endl;
		return 1;
    	}
        line1 = 0;
        while(!INFILE1.eof())
    	{
            	line1++;
            	INFILE1>>PC;
            	INFILE1>>isb;
            	INFILE1>>bo;
                pc_v[PC] = 1;
      	}
    	INFILE1.seekg(0, ios::beg);
    	INFILE1.close();
        //cout<<"number of lines: " << line1 - 1<<endl; 
	for(int test_pc = 0; test_pc < line1 - 1; test_pc++)
	{	
		for(int j = 0; j < 3; j++)
		{        
                        //cout<<"test_pc: " << test_pc << "j: " << j <<endl;  
    			outfile_name = "./" + benchmark_name + "/" + to_string(i) + "/headroom/pc_choice" + to_string(test_pc) + "_" +to_string(j) + ".csv";
  			      
        		OUTFILE.open(outfile_name.c_str());
    			if(OUTFILE.fail()) 
    			{
        			cout<<"fail to open outfile:"<< outfile_name<<endl;
				return 1;
                        }
                        int cnt = 0;
                        for(map<Uint64, int>::iterator it = pc_v.begin(); it != pc_v.end(); it++)
			{
				isb = (test_pc == cnt)? j / 2 : 1;
				bo = (test_pc == cnt)? j % 2 : 1;
				OUTFILE<<it->first << " " << isb << " " << bo <<endl;
 		                cnt++;
			}
			OUTFILE.close();
		}
	}
        pc_v.clear();
    }

    return 0;
}
