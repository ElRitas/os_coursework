#!/bin/bash
rm -r *.aux *.bbl *.toc *.bcf *.blg *.log
pdflatex -interaction=nonstopmode -halt-on-error -file-line-error -output-directory "." ./report.tex
bibtex report.aux
pdflatex -interaction=nonstopmode -halt-on-error -file-line-error -output-directory "." ./report.tex
evince report.pdf
