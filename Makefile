all:  	numatool.c
	gcc -o numatool numatool.c

dut:	dut.c
	gcc -o dut dut.c

clean:
	rm -f *.o a.out numatool dut
