#include <iostream>
#include <fstream>
#include <string>
#include <stdint.h>
#include <stdlib.h>
#include <map>
#include <string>
using namespace std;
typedef uint64_t Uint64;
typedef struct result
{
    result(){
        isb_hits = 0;
        isb_prefetches = 0;
        bo_hits = 0;
        bo_prefetches = 0;
    }
    Uint64 isb_hits;
    Uint64 isb_prefetches;
    Uint64 bo_hits;
    Uint64 bo_prefetches;
}Result;

int main(int argc, char** argv)
{
    if(argc != 3)
    {
        cout<<"usage: ./a.out benchmark [num]"<<endl;
        return 1;
    }
    string filename1, filename2;
    ifstream INFILE1;
    ifstream INFILE2;
    string string1,string2;
    map<Uint64, Result> stats;
    int line1 = 0, line2 = 0;
    Uint64 PC, hits, prefetches;
   
    int similar_high = 0, similar_mid = 0, similar_low = 0; 
    int similar_high_isb = 0, similar_mid_isb = 0, similar_low_isb = 0;
    int similar_high_bo = 0, similar_mid_bo = 0, similar_low_bo = 0;
    int isb_high = 0, isb_mid = 0, isb_low = 0;
    int bo_high = 0, bo_mid = 0, bo_low = 0;
    string benchmark_name(argv[1]);
    for(int i = 1; i <= atoi(argv[2]); i++)
    {
        filename1 = "./" + benchmark_name+ "/" + to_string(i) +"/pc_prefetches.csv";
    	INFILE1.open(filename1.c_str());
    	if(INFILE1.fail()) 
    	{
        	cout<<"fail to open file1:"<< filename1<<endl;
		return 1;
    	}
        filename2 = "../bo-akanksha/" + benchmark_name + "/" + to_string(i) +"/pc_prefetches.csv";
    	INFILE2.open(filename2.c_str());
    	if(INFILE2.fail()) 
    	{
        	cout<<"fail to open fil2"<<endl;
		return 1;
    	}
    	while(!INFILE1.eof())
    	{
        	INFILE1>>string1;
        	if(string1 == "PC:")
        	{
            		line1++;
            		INFILE1>>PC;
            		INFILE1>>string1;
            		INFILE1>>prefetches;
            		INFILE1>>string1;
            		INFILE1>>hits;
            		if(stats.find(PC) == stats.end())
            		{
                		stats[PC] = *(new Result);
            		}	 
            		stats[PC].isb_hits += hits;
            		stats[PC].isb_prefetches += prefetches;
        	}
    	}
    	INFILE1.seekg(0, ios::beg);
    	INFILE1.close();
   
    	while(!INFILE2.eof())
    	{
        	INFILE2>>string1;
        	if(string1 == "PC:")
        	{
            		line2++;
            		INFILE2>>PC;
            		INFILE2>>string1;
            		INFILE2>>prefetches;
            		INFILE2>>string1;
            		INFILE2>>hits;
            		if(stats.find(PC) == stats.end())
            		{
                 		stats[PC] = *(new Result);
            		}     
            		stats[PC].bo_hits += hits;
            		stats[PC].bo_prefetches += prefetches;
        	}
    	}
    	INFILE2.seekg(0, ios::beg);
    	INFILE2.close();

    }

    for(map<Uint64, Result>::iterator it=stats.begin(); it != stats.end(); ++it)
    {
        if(it->second.isb_prefetches > 100 || it->second.bo_prefetches > 100){
		//cout<<it->second.isb_hits<< "/" << it->second.isb_prefetches << " "<< it->second.bo_hits<<"/"<<it->second.bo_prefetches << endl;
            double isb_accuracy = (double) it->second.isb_hits / (double) it->second.isb_prefetches;
            double bo_accuracy = (double) it->second.bo_hits/ (double) it->second.bo_prefetches;
            if(isb_accuracy - bo_accuracy > 0.1)
            {
                if(isb_accuracy > 0.67)
                    isb_high++;
                else if(isb_accuracy > 0.33)
                    isb_mid++;
                else
                    isb_low++;
            }
            else if(bo_accuracy - isb_accuracy > 0.1)
            {
                 if(bo_accuracy > 0.67)
                    bo_high++;
                 else if(bo_accuracy > 0.33)
                    bo_mid++;
                 else
                    bo_low++;
            }
            else {
                double ratio = (double) it->second.isb_prefetches / (double)it->second.bo_prefetches;
                if(ratio > 1.5)
                {
                     if(bo_accuracy > 0.67 || isb_accuracy > 0.67)
                        similar_high_isb++;
                     else if(bo_accuracy > 0.33 || isb_accuracy > 0.33)
                        similar_mid_isb++;
                     else
                        similar_low_isb++;        
                }
                else if(ratio < 0.67)
                {
		     if(bo_accuracy > 0.67 || isb_accuracy > 0.67)
                        similar_high_bo++;
                     else if(bo_accuracy > 0.33 || isb_accuracy > 0.33)
                        similar_mid_bo++;
                     else
                        similar_low_bo++;   
                }
                else{
                     if(bo_accuracy > 0.67 || isb_accuracy > 0.67)
                        similar_high++;
                     else if(bo_accuracy > 0.33 || isb_accuracy > 0.33)
                        similar_mid++;
                     else
                        similar_low++;   
                }
            }
        }
    }
    cout<<argv[1]<<":"<<endl;
    cout<<"isb high mid low:" << isb_high << " "<< isb_mid << " "<< isb_low << endl;
    cout<<"bo high mid low:" << bo_high << " "<< bo_mid << " "<< bo_low << endl;
    cout<<"similar_isb high mid low: "<<similar_high_isb << " "<< similar_mid_isb << " "<< similar_low_isb << endl;
    cout<<"similar_bo high mid low: "<<similar_high_bo << " "<< similar_mid_bo << " "<< similar_low_bo << endl;
    cout<<"similar high mid low: "<<similar_high << " "<< similar_mid << " "<< similar_low << endl;
    /*while(!INFILE2.eof())
    {
        INFILE2>>string2;
        if(string2 == "PC:")
            line2++;
    }
    INFILE2.seekg(0, ios::beg);
    cout<<line1<<" "<<line2<<endl;*/
    return 0;
}
