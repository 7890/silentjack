#!/bin/sh

#sudo apt-get install pdftk

pdftk \
	silentjack_osc.pdf \
	cat 1 3-end output \
	silentjack_osc.pdf.out \
&& mv silentjack_osc.pdf.out silentjack_osc.pdf



