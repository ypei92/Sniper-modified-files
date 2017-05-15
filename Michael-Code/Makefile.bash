#make pc_choice.csv for each pinpoint
g++ -std=c++0x test2.cpp -o a.out 
#make all the possible pc choices for each pinpoint
g++ -std=c++0x headroom_makecfg.cpp -o headroom_makecfg


g++ -std=c++0x headroom_mkcondor.cpp -o headroom_mkcondor
#choose best choice options
g++ -std=c++0x headroom_choosebest.cpp -o headroom_choosebest
g++ -std=c++0x headroom_distribution.cpp -o headroom_distribution
#bash order
#headroom_mkdir.bash build headroom dir for each pinpoint

#stats.bash execute a.out and headroom_cfg for each pinpoint, generate all possible .cfg in headroom dir
#headroom_mkdir_eachcfg.bash build dir for each possible choice, mv .cfg into them

#headroom_mkcondor.bash make run.sh and run.condor for each possible choice in the headroom/pc_choice*_*/
#headroom_runcondor.bash submit all jobs to condor

#headroom_choose_runbest_cp.bash : store current naive hybrid to /naive_hybrid/, pick the best choice and submit to condor
#headroom_choose_runbest_nocp: pick the best choice and submit to condor
