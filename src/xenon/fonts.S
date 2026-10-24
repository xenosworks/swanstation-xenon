	/* The picker's font, Archivo Narrow regular and medium (SIL Open Font License 1.1, fonts/OFL.txt), built into the
	   .xex for picker.cpp. */
	.section .rodata
	.globl font_regular, font_regular_end, font_medium, font_medium_end
	.balign 4
font_regular:
	.incbin "fonts/ArchivoNarrow-Regular.ttf"
font_regular_end:
	.balign 4
font_medium:
	.incbin "fonts/ArchivoNarrow-Medium.ttf"
font_medium_end:
