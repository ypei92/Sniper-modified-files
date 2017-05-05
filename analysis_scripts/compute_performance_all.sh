#! /bin/bash

#script_path = 
script_dir="/u/ypei/Desktop/cs395t-lin/sniper/tools" 
accuracy_file="compute_accuracy_all_hybrid.py"
coverage_file="compute_coverage_all_hybrid.py"
speedup_file="compute_speedup_all_hybrid.py"

result_dir=$1
config=$2
output_file=$3

echo "## $2 accuracy" > $output_file 
python $script_dir/$accuracy_file $result_dir $config >> $output_file
echo >> $output_file
echo "## $2 coverage" >> $output_file 
python $script_dir/$coverage_file $result_dir $config >> $output_file
echo >> $output_file
echo "## $2 speedup" >> $output_file 
python $script_dir/$speedup_file $result_dir $config >> $output_file
