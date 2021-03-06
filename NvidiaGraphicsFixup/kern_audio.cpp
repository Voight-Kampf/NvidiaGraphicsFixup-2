//
//  kern_audio.cpp
//  NvidiaGraphicsFixup
//
//  Copyright © 2017 lvs1974. All rights reserved.
//

#include <Headers/kern_iokit.hpp>
#include <Headers/plugin_start.hpp>

#include "kern_config.hpp"
#include "kern_audio.hpp"

OSDefineMetaClassAndStructors(NVidiaAudio, IOService)

uint32_t NVidiaAudio::getAnalogLayout() {
	// For some DP monitors layout-id value should match HDEF layout-id
	// If we have HDEF properly configured, get the value
	static uint32_t layout = 0;
	
	if (!layout) {
		const char *tree[] {"AppleACPIPCI"};
		auto sect = WIOKit::findEntryByPrefix("/AppleACPIPlatformExpert", "PCI", gIOServicePlane);
		for (size_t i = 0; sect && i < arrsize(tree); i++) {
			sect = WIOKit::findEntryByPrefix(sect, tree[i], gIOServicePlane);
			if (sect && i+1 == arrsize(tree)) {
				auto iterator = sect->getChildIterator(gIOServicePlane);
				if (iterator) {
					IORegistryEntry *obj = nullptr;
					while ((obj = OSDynamicCast(IORegistryEntry, iterator->getNextObject())) != nullptr) {
						uint32_t vendor = 0;
						// We can technically check the class-code too, but it is not required.
						// HDEF and HDAU should have the same layout-id if any, so it is irrelevant which we find.
						if (WIOKit::getOSDataValue(obj, "layout-id", layout) &&
							WIOKit::getOSDataValue(obj, "vendor-id", vendor) &&
							vendor == WIOKit::VendorID::Intel) {
							DBGLOG("audio", "found intel audio %s with layout-id %u", safeString(obj->getName()), layout);
							return layout;
						}
					}
					iterator->release();
				}
			}
		}
		
		DBGLOG("audio", "failed to find HDEF layout-id, falling back to 1");
		layout = 0x1;
	}
	
	return layout;
}

IOService *NVidiaAudio::probe(IOService *hdaService, SInt32 *score) {
	if (!ADDPR(startSuccess)) {
		return nullptr;
	}
    
    if (config.noaudiofixes) {
        DBGLOG("audio", "all audio fixes are disabled");
        return nullptr;
    }

	if (!hdaService) {
		DBGLOG("audio", "received null digitial audio device");
		return nullptr;
	}
	
	uint32_t hdaVen, hdaDev;
	if (!WIOKit::getOSDataValue(hdaService, "vendor-id", hdaVen) ||
		!WIOKit::getOSDataValue(hdaService, "device-id", hdaDev)) {
		SYSLOG("audio", "found an unknown device");
		return nullptr;
	}
	
	auto hdaPlaneName = safeString(hdaService->getName());
	DBGLOG("audio", "corrects digital audio for hdau at %s with %04X:%04X",
		   hdaPlaneName, hdaVen, hdaDev);
	
	if (hdaVen != VendorID::NVIDIA) {
		DBGLOG("audio", "unsupported hdau vendor");
		return nullptr;
	}
	
	IORegistryEntry *gpuService {nullptr};
	
	// Cannot iterate over siblings, so firstly get to IOPP
	auto controller = hdaService->getParentEntry(gIOServicePlane);
	if (controller) {
		// Then iterate over IOPP children (GFX0 and HDAU)
		auto iterator = controller->getChildIterator(gIOServicePlane);
		if (iterator) {
			while ((gpuService = OSDynamicCast(IORegistryEntry, iterator->getNextObject())) != nullptr) {
				uint32_t classCode;
				if (WIOKit::getOSDataValue(gpuService, "class-code", classCode)) {
					// https://pci-ids.ucw.cz/read/PD/03/00 PCI CLASS VGA COMPATIBLE CONTROLLER
					if ((classCode & 0xFFFF00) == 0x030000)
						break;
					else
						DBGLOG("audio", "found incompatible class-code %04X", classCode);
				} else {
					DBGLOG("audio", "failed to find class-code in %s", safeString(gpuService->getName()));
				}
				
				gpuService = nullptr;
			}
			
			iterator->release();
		} else {
			DBGLOG("audio", "hdau parent iterator error");
		}
	} else {
		DBGLOG("audio", "hdau parent error");
	}
	
	if (!gpuService) {
		DBGLOG("audio", "failed to find gpu service");
		return nullptr;
	}
	
	uint32_t gpuVen, gpuDev;
	if (!WIOKit::getOSDataValue(gpuService, "vendor-id", gpuVen) ||
		!WIOKit::getOSDataValue(gpuService, "device-id", gpuDev)) {
		SYSLOG("audio", "found an unknown gpu device");
		return nullptr;
	}
	
    if (gpuService->getProperty("no-audio-autofix")) {
        DBGLOG("audio", "asked to avoid messing with digital audio");
        return nullptr;
    }
    
	auto gpuPlaneName = safeString(gpuService->getName());
	DBGLOG("audio", "corrects digital audio for gpu at %s with %04X:%04X", gpuPlaneName, gpuVen, gpuDev);
	
	if (gpuVen != VendorID::NVIDIA) {
		DBGLOG("audio", "unsupported GPU vendor");
		return nullptr;
	}
    
    // Power management may cause issues for non GFXx
    if (strncmp(gpuPlaneName, "GFX", strlen("GFX"))) {
        DBGLOG("audio", "fixing gpu plane name to GFX0");
        gpuService->setName("GFX0");
    }
	
	// AppleHDAController only recognises HDEF and HDAU
	
	if (strcmp(hdaPlaneName, "HDAU")) {
		DBGLOG("audio", "fixing audio plane name to HDAU");
		hdaService->setName("HDAU");
	}
	
	// hda-gfx allows to separate the devices, must be unique
	
	auto hdaHdaGfx = hdaService->getProperty("hda-gfx");
	auto gpuHdaGfx = gpuService->getProperty("hda-gfx");
	if (!hdaHdaGfx && !gpuHdaGfx) {
		static uint32_t hdaCounter {0};
		static const char *hdaNames[] {
			"onboard-2",
			"onboard-3",
			"onboard-4"
		};
		
		if (hdaCounter < arrsize(hdaNames)) {
			DBGLOG("audio", "fixing hda-gfx to %s", hdaNames[hdaCounter]);
			auto hda = OSData::withBytes(hdaNames[hdaCounter], sizeof("onboard-2"));
			hdaService->setProperty("hda-gfx", hda);
			gpuService->setProperty("hda-gfx", hda);
			hdaCounter++;
		} else {
			SYSLOG("audio", "out of hda-gfx indexes");
		}
	} else {
		DBGLOG("audio", "existing hda-gfx in gpu (%d) or hdau (%d), assuming complete inject",
			   gpuHdaGfx != nullptr, hdaHdaGfx != nullptr);
	}
	
	// layout-id is heard to be required in rare cases
	
	if (!hdaService->getProperty("layout-id")) {
		DBGLOG("audio", "fixing layout-id in hdau");
		uint32_t layout = getAnalogLayout();
		hdaService->setProperty("layout-id", OSData::withBytes(&layout, sizeof(layout)));
	} else {
		DBGLOG("audio", "found existing layout-id in hdau");
	}
    
    if (!config.noaudioconnectors && !gpuService->getProperty("no-audio-fixconn")) {
        uint8_t builtBytes[] { 0x00, 0x08, 0x00, 0x00 };
        char connector_type[] { "@0,connector-type" };
        for (int i=0; i<MaxConnectorCount; ++i)
        {
            connector_type[1] = '0' + i;
            if (!gpuService->getProperty(connector_type)) {
                DBGLOG("audio", "fixing %s in gpu", connector_type);
                gpuService->setProperty(connector_type, OSData::withBytes(builtBytes, sizeof(builtBytes)));
            } else {
                DBGLOG("audio", "found existing %s in gpu", connector_type);
            }
        }
    }
	
	// built-in is required for non-renamed devices
	
	if (!hdaService->getProperty("built-in")) {
		DBGLOG("audio", "fixing built-in in hdau");
		uint8_t builtBytes[] { 0x01, 0x00, 0x00, 0x00 };
		hdaService->setProperty("built-in", OSData::withBytes(builtBytes, sizeof(builtBytes)));
	} else {
		DBGLOG("audio", "found existing built-in in hdau");
	}
	
	if (!gpuService->getProperty("built-in")) {
		DBGLOG("audio", "fixing built-in in gpu");
		uint8_t builtBytes[] { 0x01, 0x00, 0x00, 0x00 };
		gpuService->setProperty("built-in", OSData::withBytes(builtBytes, sizeof(builtBytes)));
	} else {
		DBGLOG("audio", "found existing built-in in gpu");
	}
	
    auto gpuSlotName = gpuService->getProperty("AAPL,slot-name");
    if (!gpuSlotName) {
        static uint32_t slotCounter {0};
        static const char *slotNames[] {
            "Slot-1",
            "Slot-2",
            "Slot-3",
			"Slot-4"
        };
        
        if (slotCounter < arrsize(slotNames)) {
            DBGLOG("audio", "fixing AAPL,slot-name to %s", slotNames[slotCounter]);
            auto slot = OSData::withBytes(slotNames[slotCounter], sizeof("Slot-2"));
            gpuService->setProperty("AAPL,slot-name", slot);
            slotCounter++;
        } else {
            SYSLOG("audio", "out of slot name indexes");
        }
    } else {
        DBGLOG("audio", "existing AAPL,slot-name in gpu (%d), assuming complete inject", gpuSlotName != nullptr);
    }
	
	// This may be required for device matching
	
	auto compatibleProp = OSDynamicCast(OSData, hdaService->getProperty("compatible"));
	if (compatibleProp) {
		uint32_t compatibleSz = compatibleProp->getLength();
		auto compatibleStr = static_cast<const char *>(compatibleProp->getBytesNoCopy());
		DBGLOG("audio", "compatible property starts with %s and is %u bytes", safeString(compatibleStr), compatibleSz);
		
		if (compatibleStr) {
			for (uint32_t i = 0; i < compatibleSz; i++) {
				if (!strcmp(&compatibleStr[i], "HDAU")) {
					DBGLOG("audio", "found HDAU in compatible, ignoring");
					return nullptr;
				}
				
				i += strlen(&compatibleStr[i]);
			}
			
			uint32_t compatibleBufSz = compatibleSz + sizeof("HDAU");
			uint8_t *compatibleBuf = Buffer::create<uint8_t>(compatibleBufSz);
			if (compatibleBuf) {
				DBGLOG("audio", "fixing compatible to have HDAU");
				lilu_os_memcpy(&compatibleBuf[0], compatibleStr, compatibleSz);
				lilu_os_memcpy(&compatibleBuf[compatibleSz], "HDAU", sizeof("HDAU"));
				hdaService->setProperty("compatible", OSData::withBytes(compatibleBuf, compatibleBufSz));
			} else {
				SYSLOG("audio", "compatible property memory alloc failure %u", compatibleBufSz);
			}
		}
	} else {
		SYSLOG("audio", "compatible property is missing");
	}

	return nullptr;
}
