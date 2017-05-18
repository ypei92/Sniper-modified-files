Hi, this directory includes my (Yan Pei) source code to do the headroom
experiment and the dynamic hybrid prefetcher.

For the static analysis headroom test, I give a sample command for running this.
$ gcc headroom_test.c -o headroom_test && ./headroom_test output/run_all_isb output/run_all_bo output/run_all_naive_hybrid output/run_all_test_dir

You should first replace the cache_cntlr with what I have in the sniper
directory.

For the hybrid prefetcher, you can just put the 6 files in this sniper
directory to the real sniper directory. I defined a lot of MACROs that you can
change to achieve different behaviour.

If you have questions about my code, please contact me!!

Yan Pei
