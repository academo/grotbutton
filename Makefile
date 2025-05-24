upload:
	-pkill -f pio
	pio run --target upload
	# call monitor
	${MAKE} monitor

monitor:
	-pkill -f pio
	pio device monitor

