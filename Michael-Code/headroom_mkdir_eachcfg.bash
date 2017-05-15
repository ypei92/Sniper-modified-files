#remove all dir in headroom and rebuild them
benchmark=(
"astar"
"bzip2"
"gcc"
"libquantum"
"mcf"
"omnetpp"
"perlbench"
"sjeng"
"soplex"
"sphinx3"
"xalancbmk"
)
for i in "${benchmark[@]}";
do
    cd "$i"
    echo "$i"
    for j in */; 
    do
        cd "$j" 
        #echo "$j"
	cd "headroom"
        
        for cfg in *;
        do
            export TEMP_DIR=$(echo $cfg | sed 's/\.csv//g') 
	    mkdir $TEMP_DIR
            mv $cfg $TEMP_DIR/pc_choice.csv
        done
        cd ../..
    done
    cd ..
#>> stats/summary.stats
done



