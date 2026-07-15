use case:
- backup+storage
- web hosting
- synchronization
- docker containers for hobby projects
- vpn server
- coding agents
- video library + streaming
- building images/binaries
- just generally having access to a machine that i can trust to run 24/7. sometimes i have jobs running on my laptop so i cant put it in my backpack without pausing them..

most of this stuff is covered by my synology nas, the vpn will be done by the router, the coding agents will run on the pc (but no local inference for noe until gpus bacome cheap again).

requirements:
- 10 inch "standard"
- 8u or taller
- must fit:
	- my ds223j 2-bay synology nas
	- min 8-port switch (min 1gbit)
	- ups+psu
	- router
	- some computer / sbc
	- kvm
	- some way of remote power cycling (can def build that myself with some relays if cheaper than some high end device)
	- ports for peripherals/external drives (on pc probably)
	- cooling fans + basic filters
	- empty space for upgrades
	- current draw sensor
- nice to have:
	- wireless connection
	- spot for meshtastic node
	- ac outlet bar
	- all internal supplies on dc (higher efficiency for longer ups runtime)
	- bluetooth/audio
	- 2.5gbit+ ethernet
	- sfp uplink
	- poe switch
	- power conditioning
	- long battery life (1+ day)
	- alternate power source options. e.g. solar, cat vattery, etc
	- small display
	- drawer for tools etc
	- temperature/humidity/battery voltage/input power characteristics/vibration sensors/microphone (quite effective for failing fans/hdds)/imu (tipping/tilting/lifting detection)/differential pressure between intake and exhaust (detect clogged filters)
	- buzzer/speaker
	- control mcu with RTC


after a while of looking for components i think the setup should look roughly like this:
- mini itx mainboard
- PicoPSU for powering the mainboard off of 12v
- DIY UPS. sounds stupid, maybe it is. my reasoning:
  - the options out there suck complete ass and i hate to pay 150 bucks for a small battery with a huge inverter that i dont need and that reduces my efficiency.
  - the synology nas and the pico psu both require 12v. the pico psu goes down to a little further than 12v even according to the spec, i suspect the nas does too, so we should be in the clear if the battery dips below 12v briefly.
  - so, effectively all i need is 12v batteries with a battery charger/manager.
  - if the proximity to the 12v floor is problematic, then id put the batteries in series and step down to exact 12v, but that ofc reduces efficiency.
- as for the mainboard
  - id either go with a CWWK-brand board that already ships with an N100/i5-8265U (around 200eur), or id just pick a regular mini itx board and put some amd cpu in it. thats probably 100 eur more expensive but i get overproportionately more performance. *but*, i'd still want a cpu that can go low in terms of tdp, that server is gonna be running 24/7 after all and energy prices need to be considered (maybe do a quick calculation/comparison between a beefier cpu and the n100-class boards with the prices in upper austria)
- already got sodim ram and m.2 ssd, so i win by using boards like the ones i mentioned because they dont solder those on.
- i'll add a little mcu with a bunch of sensors that i'll hook up via spi/i2c. most mainboards expose some interface like that.
- maybe i'll throw in some SBCs that i have laying around
- mainboard will also act as openwrt router, so one less component and (as far as i can tell) no real reason not to do this. correct me if im not seeing something (note: the idea is that the whole rack is its own network and the router is the root of that network. i do want isolation from the outside, and full network configurability on the inside, useful when the rack is hooked up to some foreign network)
- the switch:
  - either i go non-poe, which means i can pick a switch that works at around 12v, or i go poe, which means i'll prob need around 48v. in that case i need batteries in series and a dc-dc step-down to 12v. maybe actually the more flexible option, even if i dont go with poe. stepping 12v up is likely more of a hassle and more inefficient than stepping sth like 48v down.