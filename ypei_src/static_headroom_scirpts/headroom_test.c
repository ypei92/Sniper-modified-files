#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <dirent.h> 
#include <string.h> 
//#include <fstream>

#define DEBUG 1
#define NEITHER 0
#define FIRST 1     //isb
#define SECOND 2    //bo
#define BOTH 3

#define COMPARE_ACCURACY 0
#define COMPARE_HIT_MISS_PREFETCH 0
#define COMPARE_SHARED_HIT_RATE 1

const char* reference_name = {"pc_prefetches.csv"};
const char* decision_name = {"decision.csv"};
const char benchmark_set[16][16] = {"astar", "bzip2", "gcc", "libquantum", "mcf", 
                          "omnetpp", "perlbench", "soplex", "sphinx3", "xalancbmk"};
const int benchmark_number = 10;
const int pin_point_max = 32; 
// const double accuracy_threshold = 0.1;
const double accuracy_threshold = 0.2;

const char* locate_pc_str = "PC: ";
const char* locate_prefetches_str = "prefetches: ";
const char* locate_prefetch_hits_str = "prefetch_hits: ";
const char* locate_accuracy_str = "accuracy: ";

int FindMatchedPC(int, int (*)[3], int);
int FindShared(int, int, int);

int main(int argc, char* argv[]) {
    if (argc != 5) {
        printf("Format: ./headroom_test isb_dir0 bo_dir1 hybrid_baseline_dir target_dir\n");
        return -1;
    }

    int input0pclist[4096][3] = {0};
    int len_input0pclist = 0;
    int input1pclist[4096][3] = {0};
    int len_input1pclist = 0;
    int input2pclist[4096][3] = {0};
    int len_input2pclist = 0;

    int i, j, k, l;
    DIR *dir_pin_point = NULL;
    FILE* inp0, *inp1, *inp2, *out;
    char* inputfile0path = (char*)malloc(128);
    char* inputfile1path = (char*)malloc(128);
    char* inputfile2path = (char*)malloc(128);
    char* outputfilepath = (char*)malloc(128);
    for (i = 0; i < benchmark_number; i++) {
        for (j = 0; j < pin_point_max; j++) { 
            memset(inputfile0path, 0, sizeof(inputfile0path));
            memset(inputfile1path, 0, sizeof(inputfile1path));
            memset(inputfile1path, 0, sizeof(inputfile2path));
            memset(outputfilepath, 0, sizeof(outputfilepath));

            sprintf(outputfilepath, "%s/%s/%d/", argv[4], benchmark_set[i], j);
            if ((dir_pin_point = opendir(outputfilepath)) == NULL) {
                continue;
            }

            closedir(dir_pin_point);
            sprintf(inputfile0path, "%s/%s/%d/%s", argv[1], benchmark_set[i], j, reference_name);
            sprintf(inputfile1path, "%s/%s/%d/%s", argv[2], benchmark_set[i], j, reference_name);
            sprintf(inputfile2path, "%s/%s/%d/%s", argv[3], benchmark_set[i], j, reference_name);
            strcat(outputfilepath, decision_name );

            /* processing .csv files */
            inp0 = fopen(inputfile0path, "r");
            inp1 = fopen(inputfile1path, "r");
            inp2 = fopen(inputfile2path, "r");
            out =  fopen(outputfilepath, "w");

            char* line = NULL;
            size_t line_buflen;
            ssize_t line_length;

            char num_str[32];
            /* for isb pc_prefetches.csv */
            while ((line_length = getline(&line, &line_buflen, inp0)) != -1) {

                char* comma = line;
                char* space = NULL;

                for (k = 0; k < 3; k++) {
                    comma = strchr(comma + 1, ':');
                    space = strchr(comma + 2, ' ');
                    strncpy(num_str, comma + 2, space - comma - 2); 
                    num_str[space - comma - 2] = '\0';
                    input0pclist[len_input0pclist][k] = atoi(num_str);
                }
#if DEBUG
                printf("inp0: line = %s", line);
                printf("inp0: pc=%d, pref=%d, hits=%d\n", input0pclist[len_input0pclist][0],
                        input0pclist[len_input0pclist][1], input0pclist[len_input0pclist][2]);
#endif
                ++len_input0pclist;
            }

            /* for bo pc_prefetches.csv */
            while ((line_length = getline(&line, &line_buflen, inp1)) != -1) {

                char* comma = line;
                char* space = NULL;

                for (k = 0; k < 3; k++) {
                    comma = strchr(comma + 1, ':');
                    space = strchr(comma + 2, ' ');
                    strncpy(num_str, comma + 2, space - comma - 2); 
                    num_str[space - comma - 2] = '\0';
                    input1pclist[len_input1pclist][k] = atoi(num_str);
                }
#if DEBUG
                printf("inp1: line = %s", line);
                printf("inp1: pc=%d, pref=%d, hits=%d\n", input1pclist[len_input1pclist][0],
                        input1pclist[len_input1pclist][1], input1pclist[len_input1pclist][2]);
#endif
                ++len_input1pclist;
            }

            /* for hybrid baseline pc_prefetches.csv */
            while ((line_length = getline(&line, &line_buflen, inp2)) != -1) {

                char* comma = line;
                char* space = NULL;

                for (k = 0; k < 3; k++) {
                    comma = strchr(comma + 1, ':');
                    space = strchr(comma + 2, ' ');
                    strncpy(num_str, comma + 2, space - comma - 2); 
                    num_str[space - comma - 2] = '\0';
                    input2pclist[len_input2pclist][k] = atoi(num_str);
                }
#if DEBUG
                printf("inp2: line = %s", line);
                printf("inp2: pc=%d, pref=%d, hits=%d\n", input2pclist[len_input2pclist][0],
                        input2pclist[len_input2pclist][1], input2pclist[len_input2pclist][2]);
#endif
                ++len_input2pclist;
            }


            for ( k = 0; k < len_input2pclist; k++) {
                if (input2pclist[k][0] == 0) {
                    continue;
                }

                int pc = input2pclist[k][0];
                int index0 = FindMatchedPC(pc, input0pclist, len_input0pclist);
                int index1 = FindMatchedPC(pc, input1pclist, len_input1pclist);

                if (index0 == -1 || index1 == -1) {
                    int* pc_data = (index0 != -1)?input0pclist[index0]:input1pclist[index1]; 
                    double pc_accuracy = ((double)pc_data[2])/pc_data[1];
                    
                    if (pc_accuracy > accuracy_threshold) {
                        fprintf(out, "pc:%d decision:%d\n", pc, (index0 != -1)?FIRST:SECOND);
                    } else {
                        fprintf(out, "pc:%d decision:%d\n", pc, NEITHER);
                    }
                    continue;
                }

                int* pc_data_isb = input0pclist[index0]; 
                int* pc_data_bo = input1pclist[index1]; 
                int* pc_data_hy = input2pclist[k]; 

                double accu_isb = (double)pc_data_isb[2]/pc_data_isb[1];
                double accu_bo = (double)pc_data_bo[2]/pc_data_bo[1];
                double accu_hy = (double)pc_data_hy[2]/pc_data_hy[1];
#if COMPARE_ACCURACY
                /* compare accuracy using data from isb, bo and hybrid_baseline: headroom1 */
                if (accu_hy >= accu_isb && accu_hy >= accu_isb && accu_hy > accuracy_threshold) {
                    fprintf(out, "pc:%d decision:%d\n", pc, BOTH);
                } else if (accu_isb >= accu_hy && accu_hy >= accu_bo && accu_isb > accuracy_threshold) {
                    fprintf(out, "pc:%d decision:%d\n", pc, FIRST);
                } else if (accu_bo >= accu_hy && accu_hy >= accu_isb && accu_bo > accuracy_threshold) {
                    fprintf(out, "pc:%d decision:%d\n", pc, SECOND);
                } else if (accu_hy <= accu_isb && accu_hy <= accu_bo && accu_hy != 0) {
                    if (accu_isb >= accu_bo && accu_isb > accuracy_threshold) {
                        fprintf(out, "pc:%d decision:%d\n", pc, FIRST);
                    } else if (accu_bo >= accu_isb && accu_bo > accuracy_threshold) {
                        fprintf(out, "pc:%d decision:%d\n", pc, SECOND);
                    } else {
                        fprintf(out, "pc:%d decision:%d\n", pc, NEITHER);
                    }
                } else {
                    fprintf(out, "pc:%d decision:%d\n", pc, NEITHER);
                }
#endif

#if COMPARE_HIT_MISS_PREFETCH
                // const double misprefetch_punish = 0.25; /*headroom2 headroom6 with threshold = 0.25 headroom7 with diff gnaddr*/
                // const double misprefetch_punish = 0.5; /*headroom3*/
                // const double misprefetch_punish = 1.0 - ((double)pc_data_hy[2]/pc_data_hy[1]); /*headroom4*/ 
                // const double misprefetch_punish = ((double)pc_data_hy[2]/pc_data_hy[1]); 
                const double misprefetch_punish = 2*(1.0 - ((double)pc_data_hy[2]/pc_data_hy[1])); /*headroom5*/
                double score_isb = pc_data_isb[2] - (pc_data_isb[1] - pc_data_isb[2])*misprefetch_punish;
                double score_bo = pc_data_bo[2] - (pc_data_bo[1] - pc_data_bo[2])*misprefetch_punish;
                double score_hy = pc_data_hy[2] - (pc_data_hy[1] - pc_data_hy[2])*misprefetch_punish;

                if (accu_hy >= accu_isb && accu_hy >= accu_isb && accu_hy > accuracy_threshold) {
                    fprintf(out, "pc:%d decision:%d\n", pc, BOTH);
                } else if (score_hy >= score_isb && score_hy >= score_bo && accu_hy > accuracy_threshold) {
                    fprintf(out, "pc:%d decision:%d\n", pc, BOTH);
                } else if (score_isb >= score_hy && score_hy >= score_bo && accu_isb > accuracy_threshold) {
                    fprintf(out, "pc:%d decision:%d\n", pc, FIRST);
                } else if (score_bo >= score_hy && score_hy >= score_isb && accu_bo > accuracy_threshold) {
                    fprintf(out, "pc:%d decision:%d\n", pc, SECOND);
                } else if (score_hy <= score_isb && score_hy <= score_bo && accu_hy != 0) {
                    if (score_isb >= score_bo && accu_isb > accuracy_threshold) {
                        fprintf(out, "pc:%d decision:%d\n", pc, FIRST);
                    } else if (score_bo >= score_isb && accu_bo > accuracy_threshold) {
                        fprintf(out, "pc:%d decision:%d\n", pc, SECOND);
                    } else {
                        fprintf(out, "pc:%d decision:%d\n", pc, NEITHER);
                    }
                } else {
                    fprintf(out, "pc:%d decision:%d\n", pc, NEITHER);
                }
#endif

#if COMPARE_SHARED_HIT_RATE
                int shared_hit = FindShared(pc_data_bo[2], pc_data_isb[2], pc_data_hy[2]);
                int shared_pre = FindShared(pc_data_bo[1], pc_data_isb[1], pc_data_hy[1]);
                int shared_mispre = FindShared(pc_data_bo[1] - pc_data_bo[2],
                                               pc_data_isb[1] - pc_data_isb[2],
                                               pc_data_hy[1] - pc_data_hy[2]);

                printf("pc = %d, share_hit = %d, shard_mispre = %d, share_pre = %d\n",
                        pc, shared_hit, shared_mispre, shared_pre);
                printf("hy  hit = %d, pre = %d, acc = %f\n", pc_data_hy[2], pc_data_hy[1], accu_hy);
                printf("isb hit = %d, pre = %d, acc = %f\n", pc_data_isb[2], pc_data_isb[1], accu_isb);
                printf("bo  hit = %d, pre = %d, acc = %f\n", pc_data_bo[2], pc_data_bo[1], accu_bo);

                const double misprefetch_punish = (1.0 - accu_hy); /*headroom5*/
                double score_isb = pc_data_isb[2] - (pc_data_isb[1] - pc_data_isb[2])*misprefetch_punish;
                double score_bo = pc_data_bo[2] - (pc_data_bo[1] - pc_data_bo[2])*misprefetch_punish;

                const double both_thres = 0.7; //low_headroom9
                const double neither_thres = 0.2;
                // const double both_thres = 0.6; //low_headroom8
                // const double neither_thres = 0.1;

                int choice;
                if (accu_hy > both_thres) {
                    choice = 3; 
                    fprintf(out, "pc:%d decision:%d\n", pc, BOTH);
                } else if (accu_hy < neither_thres) {
                    // another option is to use score strats
                    if (accu_isb >= accu_bo && accu_isb > accuracy_threshold) {
                        choice = 1; 
                        fprintf(out, "pc:%d decision:%d\n", pc, FIRST);
                    } else if (accu_bo >= accu_isb && accu_bo > accuracy_threshold) {
                        choice = 2; 
                        fprintf(out, "pc:%d decision:%d\n", pc, SECOND);
                    } else {
                        choice = 0; 
                        fprintf(out, "pc:%d decision:%d\n", pc, NEITHER);
                    }
                } else {
                    if (accu_bo < accuracy_threshold && accu_isb > accuracy_threshold) {
                        choice = 1; 
                        fprintf(out, "pc:%d decision:%d\n", pc, FIRST);
                    } else if (accu_bo > accuracy_threshold && accu_isb < accuracy_threshold) {
                        choice = 2; 
                        fprintf(out, "pc:%d decision:%d\n", pc, SECOND);
                    } else {
                        double shared_score = shared_hit - accu_hy*shared_pre;
                        if (shared_score <= 0) {
                            choice = 3; 
                            fprintf(out, "pc:%d decision:%d\n", pc, BOTH);
                        } else {
                            if (score_isb >= score_bo) {
                                choice = 1; 
                                fprintf(out, "pc:%d decision:%d\n", pc, FIRST);
                            } else {
                                choice = 2; 
                                fprintf(out, "pc:%d decision:%d\n", pc, SECOND);
                            }
                        }
                    }
                }
                printf("choice = %d\n", choice);

#endif
            }

            len_input0pclist = 0;
            len_input1pclist = 0;
            len_input2pclist = 0;

            fclose(inp0);
            fclose(inp1);
            fclose(inp2);
            fclose(out);

#if DEBUG
            printf("inputfile0path = %s\n", inputfile0path);
            printf("inputfile1path = %s\n", inputfile1path);
            printf("inputfile2path = %s\n", inputfile2path);
            printf("outputfilepath = %s\n", outputfilepath);
#endif

        }
    }

    free(inputfile0path);
    free(inputfile1path);
    free(inputfile2path);
    free(outputfilepath);

    return 0;
}

int FindMatchedPC(int pc, int (*m)[3], int len_m) {
    int index = -1;
    int i = 0; 
    for(i = 0; i < len_m; i++) {
        if (m[i][0] == pc) {
            index = i;
            break;
        }
    }

    return index;
}

int FindShared(int bo, int isb, int hybrid) {
    int shared = bo + isb - hybrid;

    if (shared >= bo || shared >= isb) {
        shared = (bo < isb)?bo:isb;
    }

    return shared;
}
