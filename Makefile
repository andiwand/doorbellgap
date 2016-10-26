# See avr-help for all possible devices
DEVICE = attiny85
CLOCK = 16000000
PROGRAMMER = -c avrisp -b 19200 -P /dev/ttyACM0
CSTANDARD = gnu99

OBJECTS = main.o

# fuse settings:
# use http://www.engbedded.com/fusecalc
#FUSES = -U lfuse:w:0x62:m -U hfuse:w:0xdf:m -U efuse:w:0xff:m # 1mhz
#FUSES = -U lfuse:w:0xe2:m -U hfuse:w:0xdf:m -U efuse:w:0xff:m # 8mhz
FUSES = -U lfuse:w:0xe1:m -U hfuse:w:0xdf:m -U efuse:w:0xff:m # 16mhz

AVRDUDE = avrdude $(PROGRAMMER) -p $(DEVICE)
COMPILE = avr-gcc -Wall -std=$(CSTANDARD) -Os -DF_CPU=$(CLOCK) -mmcu=$(DEVICE)

all: hex

install: flash fuse

.c.o:
	$(COMPILE) -c $< -o $@

.S.o:
	$(COMPILE) -x assembler-with-cpp -c $< -o $@

.c.s:
	$(COMPILE) -S $< -o $@

flash: hex
	$(AVRDUDE) -v -U flash:w:main.flash.hex -U eeprom:w:main.eeprom.hex

fuse:
	$(AVRDUDE) $(FUSES)

clean:
	rm -f main.hex main.elf $(OBJECTS)

main.elf: $(OBJECTS)
	$(COMPILE) -o main.elf $(OBJECTS)

hex: main.elf
	rm -f main.flash.hex main.eeprom.hex
	avr-objcopy -j .text -j .data -O ihex main.elf main.flash.hex
	avr-objcopy -j .eeprom --set-section-flags=.eeprom="alloc,load" --change-section-lma .eeprom=0 -O ihex main.elf main.eeprom.hex
	avr-size --format=avr --mcu=$(DEVICE) main.elf

backup:
	#calibration eeprom efuse flash fuse hfuse lfuse lock signature application apptable boot prodsig usersig
	@for memory in flash eeprom; do \
		$(AVRDUDE) -v -U $(memory):r:$(DEVICE).$(memory).hex:i; \
	done

disassemble: main.elf
	avr-objdump -s -j .fuse main.elf
	avr-objdump -C -d main.elf 2>&1

dumpelf: main.elf
	avr-objdump -s -h main..elf

