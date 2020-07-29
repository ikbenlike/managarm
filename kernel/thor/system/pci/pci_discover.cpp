#include <algorithm>
#include <frigg/debug.hpp>
#include <hw.frigg_pb.hpp>
#include <mbus.frigg_pb.hpp>
#ifdef __x86_64__
#include <thor-internal/arch/pic.hpp>
#endif
#include <thor-internal/fiber.hpp>
#include <thor-internal/io.hpp>
#include <thor-internal/kernel_heap.hpp>
#include <thor-internal/address-space.hpp>
#include <thor-internal/acpi/acpi.hpp>
#include <thor-internal/framebuffer/boot-screen.hpp>
#include <thor-internal/pci/pci.hpp>

#include <lai/core.h>
#include <lai/helpers/pci.h>

namespace thor {

// TODO: Move this to a header file.
extern frigg::LazyInitializer<LaneHandle> mbusClient;

namespace pci {

frigg::LazyInitializer<frigg::Vector<frigg::SharedPtr<PciDevice>, KernelAlloc>> allDevices;

namespace {
	coroutine<bool> handleReq(LaneHandle lane, frigg::SharedPtr<PciDevice> device) {
		auto [acceptError, conversation] = co_await AcceptSender{lane};
		if(acceptError == Error::endOfLane)
			co_return false;
		// TODO: improve error handling here.
		assert(acceptError == Error::success);

		auto [reqError, reqBuffer] = co_await RecvBufferSender{conversation};
		// TODO: improve error handling here.
		assert(reqError == Error::success);

		managarm::hw::CntRequest<KernelAlloc> req(*kernelAlloc);
		req.ParseFromArray(reqBuffer.data(), reqBuffer.size());

		if(req.req_type() == managarm::hw::CntReqType::GET_PCI_INFO) {
			managarm::hw::SvrResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::hw::Errors::SUCCESS);

			for(size_t i = 0; i < device->caps.size(); i++) {
				managarm::hw::PciCapability<KernelAlloc> msg(*kernelAlloc);
				msg.set_type(device->caps[i].type);
				msg.set_offset(device->caps[i].offset);
				msg.set_length(device->caps[i].length);
				resp.add_capabilities(std::move(msg));
			}

			for(size_t k = 0; k < 6; k++) {
				managarm::hw::PciBar<KernelAlloc> msg(*kernelAlloc);
				if(device->bars[k].type == PciDevice::kBarIo) {
					assert(device->bars[k].offset == 0);
					msg.set_io_type(managarm::hw::IoType::PORT);
					msg.set_address(device->bars[k].address);
					msg.set_length(device->bars[k].length);
				}else if(device->bars[k].type == PciDevice::kBarMemory) {
					msg.set_io_type(managarm::hw::IoType::MEMORY);
					msg.set_address(device->bars[k].address);
					msg.set_length(device->bars[k].length);
					msg.set_offset(device->bars[k].offset);
				}else{
					assert(device->bars[k].type == PciDevice::kBarNone);
					msg.set_io_type(managarm::hw::IoType::NO_BAR);
				}
				resp.add_bars(std::move(msg));
			}

			frg::string<KernelAlloc> ser(*kernelAlloc);
			resp.SerializeToString(&ser);
			frigg::UniqueMemory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
			memcpy(respBuffer.data(), ser.data(), ser.size());
			auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
			// TODO: improve error handling here.
			assert(respError == Error::success);
		}else if(req.req_type() == managarm::hw::CntReqType::ACCESS_BAR) {
			auto index = req.index();

			AnyDescriptor descriptor;
			if(device->bars[index].type == PciDevice::kBarIo) {
				descriptor = IoDescriptor{device->bars[index].io};
			}else{
				assert(device->bars[index].type == PciDevice::kBarMemory);
				descriptor = MemoryViewDescriptor{device->bars[index].memory};
			}

			managarm::hw::SvrResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::hw::Errors::SUCCESS);

			frg::string<KernelAlloc> ser(*kernelAlloc);
			resp.SerializeToString(&ser);
			frigg::UniqueMemory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
			memcpy(respBuffer.data(), ser.data(), ser.size());
			auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
			// TODO: improve error handling here.
			assert(respError == Error::success);

			auto descError = co_await PushDescriptorSender{conversation, std::move(descriptor)};
			// TODO: improve error handling here.
			assert(descError == Error::success);
		}else if(req.req_type() == managarm::hw::CntReqType::ACCESS_IRQ) {
			managarm::hw::SvrResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::hw::Errors::SUCCESS);

			assert(device->interrupt);
			auto object = frigg::makeShared<IrqObject>(*kernelAlloc,
					frigg::String<KernelAlloc>{*kernelAlloc, "pci-irq."}
					+ frigg::to_string(*kernelAlloc, device->bus)
					+ frigg::String<KernelAlloc>{*kernelAlloc, "-"}
					+ frigg::to_string(*kernelAlloc, device->slot)
					+ frigg::String<KernelAlloc>{*kernelAlloc, "-"}
					+ frigg::to_string(*kernelAlloc, device->function));
			IrqPin::attachSink(device->interrupt, object.get());

			frg::string<KernelAlloc> ser(*kernelAlloc);
			resp.SerializeToString(&ser);
			frigg::UniqueMemory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
			memcpy(respBuffer.data(), ser.data(), ser.size());
			auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
			// TODO: improve error handling here.
			assert(respError == Error::success);

			auto descError = co_await PushDescriptorSender{conversation, IrqDescriptor{object}};
			// TODO: improve error handling here.
			assert(descError == Error::success);
		}else if(req.req_type() == managarm::hw::CntReqType::CLAIM_DEVICE) {
			if(device->associatedScreen) {
				frigg::infoLogger() << "thor: Disabling screen associated with PCI device "
						<< device->bus << "." << device->slot << "." << device->function
						<< frigg::endLog;
				disableLogHandler(device->associatedScreen);
			}

			managarm::hw::SvrResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::hw::Errors::SUCCESS);

			frg::string<KernelAlloc> ser(*kernelAlloc);
			resp.SerializeToString(&ser);
			frigg::UniqueMemory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
			memcpy(respBuffer.data(), ser.data(), ser.size());
			auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
			// TODO: improve error handling here.
			assert(respError == Error::success);
		}else if(req.req_type() == managarm::hw::CntReqType::BUSIRQ_ENABLE) {
			auto command = readPciHalf(device->bus, device->slot, device->function, kPciCommand);
			writePciHalf(device->bus, device->slot, device->function,
					kPciCommand, command & ~uint16_t{0x400});

			managarm::hw::SvrResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::hw::Errors::SUCCESS);

			frg::string<KernelAlloc> ser(*kernelAlloc);
			resp.SerializeToString(&ser);
			frigg::UniqueMemory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
			memcpy(respBuffer.data(), ser.data(), ser.size());
			auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
			// TODO: improve error handling here.
			assert(respError == Error::success);
		}else if(req.req_type() == managarm::hw::CntReqType::BUSMASTER_ENABLE) {
			auto command = readPciHalf(device->bus, device->slot, device->function, kPciCommand);
			writePciHalf(device->bus, device->slot, device->function,
					kPciCommand, command | 0x0004);

			managarm::hw::SvrResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::hw::Errors::SUCCESS);

			frg::string<KernelAlloc> ser(*kernelAlloc);
			resp.SerializeToString(&ser);
			frigg::UniqueMemory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
			memcpy(respBuffer.data(), ser.data(), ser.size());
			auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
			// TODO: improve error handling here.
			assert(respError == Error::success);
		}else if(req.req_type() == managarm::hw::CntReqType::LOAD_PCI_SPACE) {
			// TODO: Perform some sanity checks on the offset.
			uint32_t word;
			if(req.size() == 1) {
				word = readPciByte(device->bus, device->slot, device->function, req.offset());
			}else if(req.size() == 2) {
				word = readPciHalf(device->bus, device->slot, device->function, req.offset());
			}else{
				assert(req.size() == 4);
				word = readPciWord(device->bus, device->slot, device->function, req.offset());
			}

			managarm::hw::SvrResponse<KernelAlloc> resp{*kernelAlloc};
			resp.set_error(managarm::hw::Errors::SUCCESS);
			resp.set_word(word);

			frg::string<KernelAlloc> ser(*kernelAlloc);
			resp.SerializeToString(&ser);
			frigg::UniqueMemory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
			memcpy(respBuffer.data(), ser.data(), ser.size());
			auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
			// TODO: improve error handling here.
			assert(respError == Error::success);
		}else if(req.req_type() == managarm::hw::CntReqType::STORE_PCI_SPACE) {
			// TODO: Perform some sanity checks on the offset.
			if(req.size() == 1) {
				writePciByte(device->bus, device->slot, device->function, req.offset(), req.word());
			}else if(req.size() == 2) {
				writePciHalf(device->bus, device->slot, device->function, req.offset(), req.word());
			}else{
				assert(req.size() == 4);
				writePciWord(device->bus, device->slot, device->function, req.offset(), req.word());
			}

			managarm::hw::SvrResponse<KernelAlloc> resp{*kernelAlloc};
			resp.set_error(managarm::hw::Errors::SUCCESS);

			frg::string<KernelAlloc> ser(*kernelAlloc);
			resp.SerializeToString(&ser);
			frigg::UniqueMemory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
			memcpy(respBuffer.data(), ser.data(), ser.size());
			auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
			// TODO: improve error handling here.
			assert(respError == Error::success);
		}else if(req.req_type() == managarm::hw::CntReqType::LOAD_PCI_CAPABILITY) {
			assert(req.index() < device->caps.size());

			// TODO: Perform some sanity checks on the offset.
			uint32_t word;
			if(req.size() == 1) {
				word = readPciByte(device->bus, device->slot, device->function,
						device->caps[req.index()].offset + req.offset());
			}else if(req.size() == 2) {
				word = readPciHalf(device->bus, device->slot, device->function,
						device->caps[req.index()].offset + req.offset());
			}else{
				assert(req.size() == 4);
				word = readPciWord(device->bus, device->slot, device->function,
						device->caps[req.index()].offset + req.offset());
			}

			managarm::hw::SvrResponse<KernelAlloc> resp{*kernelAlloc};
			resp.set_error(managarm::hw::Errors::SUCCESS);
			resp.set_word(word);

			frg::string<KernelAlloc> ser(*kernelAlloc);
			resp.SerializeToString(&ser);
			frigg::UniqueMemory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
			memcpy(respBuffer.data(), ser.data(), ser.size());
			auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
			// TODO: improve error handling here.
			assert(respError == Error::success);
		}else if(req.req_type() == managarm::hw::CntReqType::GET_FB_INFO) {
			auto fb = device->associatedFrameBuffer;
			assert(fb);

			managarm::hw::SvrResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::hw::Errors::SUCCESS);
			resp.set_fb_pitch(fb->pitch);
			resp.set_fb_width(fb->width);
			resp.set_fb_height(fb->height);
			resp.set_fb_bpp(fb->bpp);
			resp.set_fb_type(fb->type);

			frg::string<KernelAlloc> ser(*kernelAlloc);
			resp.SerializeToString(&ser);
			frigg::UniqueMemory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
			memcpy(respBuffer.data(), ser.data(), ser.size());
			auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
			// TODO: improve error handling here.
			assert(respError == Error::success);
		}else if(req.req_type() == managarm::hw::CntReqType::ACCESS_FB_MEMORY) {
			auto fb = device->associatedFrameBuffer;
			assert(fb);

			MemoryViewDescriptor descriptor{fb->memory};

			managarm::hw::SvrResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::hw::Errors::SUCCESS);

			frg::string<KernelAlloc> ser(*kernelAlloc);
			resp.SerializeToString(&ser);
			frigg::UniqueMemory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
			memcpy(respBuffer.data(), ser.data(), ser.size());
			auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
			// TODO: improve error handling here.
			assert(respError == Error::success);

			auto descError = co_await PushDescriptorSender{conversation, std::move(descriptor)};
			// TODO: improve error handling here.
			assert(descError == Error::success);
		}else{
			managarm::hw::SvrResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::hw::Errors::ILLEGAL_REQUEST);

			frg::string<KernelAlloc> ser(*kernelAlloc);
			resp.SerializeToString(&ser);
			frigg::UniqueMemory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
			memcpy(respBuffer.data(), ser.data(), ser.size());
			auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
			// TODO: improve error handling here.
			assert(respError == Error::success);
		}

		co_return true;
	}

	// ------------------------------------------------------------------------
	// mbus object creation and management.
	// ------------------------------------------------------------------------

	coroutine<LaneHandle> createObject(LaneHandle mbusLane, frigg::SharedPtr<PciDevice> device) {
		auto [offerError, conversation] = co_await OfferSender{mbusLane};
		// TODO: improve error handling here.
		assert(offerError == Error::success);

		managarm::mbus::Property<KernelAlloc> subsystem_prop(*kernelAlloc);
		subsystem_prop.set_name(frg::string<KernelAlloc>(*kernelAlloc, "unix.subsystem"));
		auto &subsystem_item = subsystem_prop.mutable_item().mutable_string_item();
		subsystem_item.set_value(frg::string<KernelAlloc>(*kernelAlloc, "pci"));

		managarm::mbus::Property<KernelAlloc> bus_prop(*kernelAlloc);
		bus_prop.set_name(frg::string<KernelAlloc>(*kernelAlloc, "pci-bus"));
		auto &bus_item = bus_prop.mutable_item().mutable_string_item();
		bus_item.set_value(frg::to_allocated_string(*kernelAlloc, device->bus, 16, 2));

		managarm::mbus::Property<KernelAlloc> slot_prop(*kernelAlloc);
		slot_prop.set_name(frg::string<KernelAlloc>(*kernelAlloc, "pci-slot"));
		auto &slot_item = slot_prop.mutable_item().mutable_string_item();
		slot_item.set_value(frg::to_allocated_string(*kernelAlloc, device->slot, 16, 2));

		managarm::mbus::Property<KernelAlloc> function_prop(*kernelAlloc);
		function_prop.set_name(frg::string<KernelAlloc>(*kernelAlloc, "pci-function"));
		auto &function_item = function_prop.mutable_item().mutable_string_item();
		function_item.set_value(frg::to_allocated_string(*kernelAlloc, device->function, 16, 1));

		managarm::mbus::Property<KernelAlloc> vendor_prop(*kernelAlloc);
		vendor_prop.set_name(frg::string<KernelAlloc>(*kernelAlloc, "pci-vendor"));
		auto &vendor_item = vendor_prop.mutable_item().mutable_string_item();
		vendor_item.set_value(frg::to_allocated_string(*kernelAlloc, device->vendor, 16, 4));

		managarm::mbus::Property<KernelAlloc> dev_prop(*kernelAlloc);
		dev_prop.set_name(frg::string<KernelAlloc>(*kernelAlloc, "pci-device"));
		auto &dev_item = dev_prop.mutable_item().mutable_string_item();
		dev_item.set_value(frg::to_allocated_string(*kernelAlloc, device->deviceId, 16, 4));

		managarm::mbus::Property<KernelAlloc> rev_prop(*kernelAlloc);
		rev_prop.set_name(frg::string<KernelAlloc>(*kernelAlloc, "pci-revision"));
		auto &rev_item = rev_prop.mutable_item().mutable_string_item();
		rev_item.set_value(frg::to_allocated_string(*kernelAlloc, device->revision, 16, 2));

		managarm::mbus::Property<KernelAlloc> class_prop(*kernelAlloc);
		class_prop.set_name(frg::string<KernelAlloc>(*kernelAlloc, "pci-class"));
		auto &class_item = class_prop.mutable_item().mutable_string_item();
		class_item.set_value(frg::to_allocated_string(*kernelAlloc, device->classCode, 16, 2));

		managarm::mbus::Property<KernelAlloc> subclass_prop(*kernelAlloc);
		subclass_prop.set_name(frg::string<KernelAlloc>(*kernelAlloc, "pci-subclass"));
		auto &subclass_item = subclass_prop.mutable_item().mutable_string_item();
		subclass_item.set_value(frg::to_allocated_string(*kernelAlloc, device->subClass, 16, 2));

		managarm::mbus::Property<KernelAlloc> if_prop(*kernelAlloc);
		if_prop.set_name(frg::string<KernelAlloc>(*kernelAlloc, "pci-interface"));
		auto &if_item = if_prop.mutable_item().mutable_string_item();
		if_item.set_value(frg::to_allocated_string(*kernelAlloc, device->interface, 16, 2));

		managarm::mbus::Property<KernelAlloc> subsystem_vendor_prop(*kernelAlloc);
		subsystem_vendor_prop.set_name(frg::string<KernelAlloc>(*kernelAlloc,
				"pci-subsystem-vendor"));
		auto &subsystem_vendor_item = subsystem_vendor_prop.mutable_item().mutable_string_item();
		subsystem_vendor_item.set_value(frg::to_allocated_string(*kernelAlloc,
				device->subsystemVendor, 16, 2));

		managarm::mbus::Property<KernelAlloc> subsystem_device_prop(*kernelAlloc);
		subsystem_device_prop.set_name(frg::string<KernelAlloc>(*kernelAlloc,
				"pci-subsystem-device"));
		auto &subsystem_device_item = subsystem_device_prop.mutable_item().mutable_string_item();
		subsystem_device_item.set_value(frg::to_allocated_string(*kernelAlloc,
				device->subsystemDevice, 16, 2));

		managarm::mbus::CntRequest<KernelAlloc> req(*kernelAlloc);
		req.set_req_type(managarm::mbus::CntReqType::CREATE_OBJECT);
		req.set_parent_id(1);
		req.add_properties(std::move(subsystem_prop));
		req.add_properties(std::move(bus_prop));
		req.add_properties(std::move(slot_prop));
		req.add_properties(std::move(function_prop));
		req.add_properties(std::move(vendor_prop));
		req.add_properties(std::move(dev_prop));
		req.add_properties(std::move(rev_prop));
		req.add_properties(std::move(class_prop));
		req.add_properties(std::move(subclass_prop));
		req.add_properties(std::move(if_prop));
		req.add_properties(std::move(subsystem_vendor_prop));
		req.add_properties(std::move(subsystem_device_prop));

		if(device->associatedFrameBuffer) {
			managarm::mbus::Property<KernelAlloc> cls_prop(*kernelAlloc);
			cls_prop.set_name(frg::string<KernelAlloc>(*kernelAlloc, "class"));
			auto &cls_item = cls_prop.mutable_item().mutable_string_item();
			cls_item.set_value(frg::string<KernelAlloc>(*kernelAlloc, "framebuffer"));
			req.add_properties(std::move(cls_prop));
		}

		frg::string<KernelAlloc> ser(*kernelAlloc);
		req.SerializeToString(&ser);
		frigg::UniqueMemory<KernelAlloc> reqBuffer{*kernelAlloc, ser.size()};
		memcpy(reqBuffer.data(), ser.data(), ser.size());
		auto reqError = co_await SendBufferSender{conversation, std::move(reqBuffer)};
		// TODO: improve error handling here.
		assert(reqError == Error::success);

		auto [respError, respBuffer] = co_await RecvBufferSender{conversation};
		// TODO: improve error handling here.
		assert(respError == Error::success);
		managarm::mbus::SvrResponse<KernelAlloc> resp(*kernelAlloc);
		resp.ParseFromArray(respBuffer.data(), respBuffer.size());
		assert(resp.error() == managarm::mbus::Error::SUCCESS);

		auto [descError, descriptor] = co_await PullDescriptorSender{conversation};
		// TODO: improve error handling here.
		assert(descError == Error::success);
		assert(descriptor.is<LaneDescriptor>());
		co_return descriptor.get<LaneDescriptor>().handle;
	}

	coroutine<void> handleBind(LaneHandle objectLane, frigg::SharedPtr<PciDevice> device) {
		auto [acceptError, conversation] = co_await AcceptSender{objectLane};
		// TODO: improve error handling here.
		assert(acceptError == Error::success);

		auto [reqError, reqBuffer] = co_await RecvBufferSender{conversation};
		// TODO: improve error handling here.
		assert(reqError == Error::success);
		managarm::mbus::SvrRequest<KernelAlloc> req(*kernelAlloc);
		req.ParseFromArray(reqBuffer.data(), reqBuffer.size());
		assert(req.req_type() == managarm::mbus::SvrReqType::BIND);

		managarm::mbus::CntResponse<KernelAlloc> resp(*kernelAlloc);
		resp.set_error(managarm::mbus::Error::SUCCESS);

		frg::string<KernelAlloc> ser(*kernelAlloc);
		resp.SerializeToString(&ser);
		frigg::UniqueMemory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
		memcpy(respBuffer.data(), ser.data(), ser.size());
		auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
		// TODO: improve error handling here.
		assert(respError == Error::success);

		auto stream = createStream();
		auto descError = co_await PushDescriptorSender{conversation,
				LaneDescriptor{stream.get<1>()}};
		// TODO: improve error handling here.
		assert(descError == Error::success);

		async::detach_with_allocator(*kernelAlloc, [] (LaneHandle lane,
				frigg::SharedPtr<PciDevice> device) -> coroutine<void> {
			while(true) {
				if(!(co_await handleReq(lane, device)))
					break;
			}
		}(std::move(stream.get<0>()), std::move(device)));
	}
}

void runDevice(frigg::SharedPtr<PciDevice> device) {
	KernelFiber::run([=] {
		async::detach_with_allocator(*kernelAlloc, [] (frigg::SharedPtr<PciDevice> device)
				-> coroutine<void> {
			auto objectLane = co_await createObject(*mbusClient, device);
			while(true)
				co_await handleBind(objectLane, device);
		}(device));
	});
}

// --------------------------------------------------------
// PciBus implementation.
// --------------------------------------------------------

PciBus::PciBus(PciBridge *associatedBridge_, uint32_t busId_, lai_nsnode_t *acpiHandle_)
: associatedBridge{associatedBridge_}, busId{busId_}, acpiHandle{acpiHandle_} {
	LAI_CLEANUP_STATE lai_state_t laiState;
	lai_init_state(&laiState);

	// Look for a PRT and evaluate it.
	lai_nsnode_t *prtHandle = lai_resolve_path(acpiHandle, "_PRT");
	if(!prtHandle) {
		if(associatedBridge) {
			frigg::infoLogger() << "thor: There is no _PRT for bus " << busId_ << ";"
					" assuming expansion bridge routing" << frigg::endLog;
			for(int i = 0; i < 4; i++) {
				_bridgeIrqs[i] = associatedBridge->parentBus->resolveIrqRoute(
						associatedBridge->slot, static_cast<IrqIndex>(i + 1));
				if(_bridgeIrqs[i])
					frigg::infoLogger() << "thor:     Bridge IRQ [" << i << "]: "
							<< _bridgeIrqs[i]->name() << frigg::endLog;
			}
			_routingModel = RoutingModel::expansionBridge;
		}else{
			frigg::infoLogger() << "thor: There is no _PRT for bus " << busId_ << ";"
					" giving up IRQ routing of this bus" << frigg::endLog;
		}
		return;
	}

	LAI_CLEANUP_VAR lai_variable_t prt = LAI_VAR_INITIALIZER;
	if (lai_eval(&prt, prtHandle, &laiState)) {
		frigg::infoLogger() << "thor: Failed to evaluate _PRT;"
				" giving up IRQ routing of this bus" << frigg::endLog;
		return;
	}

	// Walk through the PRT and determine the routing.
	struct lai_prt_iterator iter = LAI_PRT_ITERATOR_INITIALIZER(&prt);
	lai_api_error_t e;
	while (!(e = lai_pci_parse_prt(&iter))) {
		assert(iter.function == -1 && "TODO: support routing of individual functions");
		auto index = static_cast<IrqIndex>(iter.pin + 1);

		frigg::infoLogger() << "    Route for slot " << iter.slot
				<< ", " << nameOf(index) << ": "
				<< "GSI " << iter.gsi << frigg::endLog;

		// In contrast to the previous ACPICA code, LAI can resolve _CRS automatically.
		// Hence, for now we do not deal with link devices.
		configureIrq(GlobalIrqInfo{iter.gsi, {
				iter.level_triggered ? TriggerMode::level : TriggerMode::edge,
				iter.active_low ? Polarity::low : Polarity::high}});
#ifdef __x86_64__
		auto pin = getGlobalSystemIrq(iter.gsi);
		_routingTable.push({static_cast<unsigned int>(iter.slot), index, pin});
#endif
	}
	_routingModel = RoutingModel::rootTable;
}

IrqPin *PciBus::resolveIrqRoute(unsigned int slot, IrqIndex index) {
	if(_routingModel == RoutingModel::rootTable) {
		auto entry = std::find_if(_routingTable.begin(), _routingTable.end(),
				[&] (const auto &ref) { return ref.slot == slot && ref.index == index; });
		if(entry == _routingTable.end())
			return nullptr;
		assert(entry->pin);
		return entry->pin;
	}else if(_routingModel == RoutingModel::expansionBridge) {
		return _bridgeIrqs[(static_cast<int>(index) - 1 + slot) % 4];
	}else{
		return nullptr;
	}
}

// --------------------------------------------------------
// Discovery functionality
// --------------------------------------------------------

size_t computeBarLength(uintptr_t mask) {
	static_assert(sizeof(long) == 8, "Fix builtin usage");
	static_assert(sizeof(uintptr_t) == 8, "Fix builtin usage");

	assert(mask);
	size_t length_bits = __builtin_ctzl(mask);
	// TODO: Bits decoded by the PCI device.
	// size_t decoded_bits = 64 - __builtin_clzl(mask);
	// FIXME: assert(__builtin_popcountl(mask) == decoded_bits - length_bits);

	return size_t(1) << length_bits;
}

frigg::LazyInitializer<frg::vector<PciBus *, KernelAlloc>> enumerationQueue;

void checkPciFunction(PciBus *bus, uint32_t slot, uint32_t function) {
	uint16_t vendor = readPciHalf(bus->busId, slot, function, kPciVendor);
	if(vendor == 0xFFFF)
		return;

	uint8_t header_type = readPciByte(bus->busId, slot, function, kPciHeaderType);
	if((header_type & 0x7F) == 0) {
		frigg::infoLogger() << "        Function " << function << ": Device";
	}else if((header_type & 0x7F) == 1) {
		uint8_t downstreamId = readPciByte(bus->busId, slot, function, kPciBridgeSecondary);
		frigg::infoLogger() << "        Function " << function
				<< ": PCI-to-PCI bridge to bus " << (int)downstreamId;
	}else{
		frigg::infoLogger() << "        Function " << function
				<< ": Unexpected PCI header type " << (header_type & 0x7F);
	}

	auto command = readPciHalf(bus->busId, slot, function, kPciCommand);
	if(command & 0x01)
		frigg::infoLogger() << " (Decodes IO)";
	if(command & 0x02)
		frigg::infoLogger() << " (Decodes Memory)";
	if(command & 0x04)
		frigg::infoLogger() << " (Busmaster)";
	if(command & 0x400)
		frigg::infoLogger() << " (IRQs masked)";
	frigg::infoLogger() << frigg::endLog;
	writePciHalf(bus->busId, slot, function, kPciCommand, command | 0x400);

	lai_nsnode_t *deviceHandle = nullptr;
	if(bus->acpiHandle) {
		LAI_CLEANUP_STATE lai_state_t laiState;
		lai_init_state(&laiState);
		deviceHandle = lai_pci_find_device(bus->acpiHandle, slot, function, &laiState);
	}
	if(deviceHandle) {
		LAI_CLEANUP_FREE_STRING char *acpiPath = lai_stringify_node_path(deviceHandle);
		frigg::infoLogger() << "            ACPI: " << const_cast<const char *>(acpiPath)
				<< frigg::endLog;
	}

	auto device_id = readPciHalf(bus->busId, slot, function, kPciDevice);
	auto revision = readPciByte(bus->busId, slot, function, kPciRevision);
	auto class_code = readPciByte(bus->busId, slot, function, kPciClassCode);
	auto sub_class = readPciByte(bus->busId, slot, function, kPciSubClass);
	auto interface = readPciByte(bus->busId, slot, function, kPciInterface);
	frigg::infoLogger() << "            Vendor/device: " << frigg::logHex(vendor)
			<< "." << frigg::logHex(device_id) << "." << frigg::logHex(revision)
			<< ", class: " << frigg::logHex(class_code)
			<< "." << frigg::logHex(sub_class)
			<< "." << frigg::logHex(interface) << frigg::endLog;

	if((header_type & 0x7F) == 0) {
		uint16_t subsystem_vendor = readPciHalf(bus->busId, slot, function, kPciRegularSubsystemVendor);
		uint16_t subsystem_device = readPciHalf(bus->busId, slot, function, kPciRegularSubsystemDevice);
//		frigg::infoLogger() << "        Subsystem vendor: 0x" << frigg::logHex(subsystem_vendor)
//				<< ", device: 0x" << frigg::logHex(subsystem_device) << frigg::endLog;

		auto status = readPciHalf(bus->busId, slot, function, kPciStatus);

		if(status & 0x08)
			frigg::infoLogger() << "\e[35m                IRQ is asserted!\e[39m" << frigg::endLog;

		auto device = frigg::makeShared<PciDevice>(*kernelAlloc, bus, bus->busId, slot, function,
				vendor, device_id, revision, class_code, sub_class, interface, subsystem_vendor, subsystem_device);

		// Find all capabilities.
		if(status & 0x10) {
			// The bottom two bits of each capability offset must be masked!
			uint8_t offset = readPciByte(bus->busId, slot, function, kPciRegularCapabilities) & 0xFC;
			while(offset != 0) {
				auto type = readPciByte(bus->busId, slot, function, offset);

				auto name = nameOfCapability(type);
				if(name) {
					frigg::infoLogger() << "            " << name << " capability"
							<< frigg::endLog;
				}else{
					frigg::infoLogger() << "            Capability of type 0x"
							<< frigg::logHex((int)type) << frigg::endLog;
				}

				// TODO: 
				size_t size = -1;
				if(type == 0x09)
					size = readPciByte(bus->busId, slot, function, offset + 2);

				device->caps.push({type, offset, size});

				uint8_t successor = readPciByte(bus->busId, slot, function, offset + 1);
				offset = successor & 0xFC;
			}
		}

		// Determine the BARs
		for(int i = 0; i < 6; i++) {
			uint32_t offset = kPciRegularBar0 + i * 4;
			uint32_t bar = readPciWord(bus->busId, slot, function, offset);
			if(bar == 0)
				continue;

			if((bar & 1) != 0) {
				uintptr_t address = bar & 0xFFFFFFFC;

				// write all 1s to the BAR and read it back to determine this its length.
				writePciWord(bus->busId, slot, function, offset, 0xFFFFFFFF);
				uint32_t mask = readPciWord(bus->busId, slot, function, offset) & 0xFFFFFFFC;
				writePciWord(bus->busId, slot, function, offset, bar);
				auto length = computeBarLength(mask);

				device->bars[i].type = PciDevice::kBarIo;
				device->bars[i].address = address;
				device->bars[i].length = length;

				device->bars[i].io = frigg::makeShared<IoSpace>(*kernelAlloc);
				for(size_t p = 0; p < length; ++p)
					device->bars[i].io->addPort(address + p);
				device->bars[i].offset = 0;

				frigg::infoLogger() << "            I/O space BAR #" << i
						<< " at 0x" << frigg::logHex(address)
						<< ", length: " << length << " ports" << frigg::endLog;
			}else if(((bar >> 1) & 3) == 0) {
				uint32_t address = bar & 0xFFFFFFF0;

				// Write all 1s to the BAR and read it back to determine this its length.
				writePciWord(bus->busId, slot, function, offset, 0xFFFFFFFF);
				uint32_t mask = readPciWord(bus->busId, slot, function, offset) & 0xFFFFFFF0;
				writePciWord(bus->busId, slot, function, offset, bar);
				auto length = computeBarLength(mask);

				device->bars[i].type = PciDevice::kBarMemory;
				device->bars[i].address = address;
				device->bars[i].length = length;

				auto offset = address & (kPageSize - 1);
				device->bars[i].memory = frigg::makeShared<HardwareMemory>(*kernelAlloc,
						address & ~(kPageSize - 1),
						(length + offset + (kPageSize - 1)) & ~(kPageSize - 1),
						CachingMode::null);
				device->bars[i].offset = offset;

				frigg::infoLogger() << "            32-bit memory BAR #" << i
						<< " at 0x" << frigg::logHex(address)
						<< ", length: " << length << " bytes" << frigg::endLog;
			}else if(((bar >> 1) & 3) == 2) {
				assert(i < 5); // Otherwise there is no next bar.
				auto high = readPciWord(bus->busId, slot, function, offset + 4);;
				auto address = (uint64_t{high} << 32) | (bar & 0xFFFFFFF0);

				// Write all 1s to the BAR and read it back to determine this its length.
				writePciWord(bus->busId, slot, function, offset, 0xFFFFFFFF);
				writePciWord(bus->busId, slot, function, offset + 4, 0xFFFFFFFF);
				uint32_t mask = (uint64_t{readPciWord(bus->busId, slot, function, offset + 4)} << 32)
						| (readPciWord(bus->busId, slot, function, offset) & 0xFFFFFFF0);
				writePciWord(bus->busId, slot, function, offset, bar);
				writePciWord(bus->busId, slot, function, offset + 4, high);
				auto length = computeBarLength(mask);

				device->bars[i].type = PciDevice::kBarMemory;
				device->bars[i].address = address;
				device->bars[i].length = length;

				auto offset = address & (kPageSize - 1);
				device->bars[i].memory = frigg::makeShared<HardwareMemory>(*kernelAlloc,
						address & ~(kPageSize - 1),
						(length + offset + (kPageSize - 1)) & ~(kPageSize - 1),
						CachingMode::null);
				device->bars[i].offset = offset;

				frigg::infoLogger() << "            64-bit memory BAR #" << i
						<< " at 0x" << frigg::logHex(address)
						<< ", length: " << length << " bytes" << frigg::endLog;
				i++;
			}else{
				assert(!"Unexpected BAR type");
			}
		}

		auto irq_index = static_cast<IrqIndex>(readPciByte(bus->busId, slot, function,
				kPciRegularInterruptPin));
		if(irq_index != IrqIndex::null) {
			auto irq_pin = bus->resolveIrqRoute(slot, irq_index);
			if(irq_pin) {
				frigg::infoLogger() << "            Interrupt: "
						<< nameOf(irq_index)
						<< " (routed to " << irq_pin->name() << ")" << frigg::endLog;
				device->interrupt = irq_pin;
			}else{
				frigg::infoLogger() << "\e[31m" "            Interrupt routing not available!"
					"\e[39m" << frigg::endLog;
			}
		}

		allDevices->push(device);
	}else if((header_type & 0x7F) == 1) {
		LAI_CLEANUP_STATE lai_state_t laiState;
		lai_init_state(&laiState);

		auto bridge = frg::construct<PciBridge>(*kernelAlloc, bus, bus->busId, slot, function);

		uint8_t downstreamId = readPciByte(bus->busId, slot, function, kPciBridgeSecondary);
		auto downstreamBus = frg::construct<PciBus>(*kernelAlloc, bridge,
				downstreamId, deviceHandle);
		enumerationQueue->push_back(std::move(downstreamBus)); // FIXME: move should be unnecessary.
	}

	// TODO: This should probably be moved somewhere else.
	if(class_code == 0x0C && sub_class == 0x03 && interface == 0x00) {
		frigg::infoLogger() << "            \e[32mDisabling UHCI SMI generation!\e[39m"
				<< frigg::endLog;
		writePciHalf(bus->busId, slot, function, 0xC0, 0x2000);
	}
}

void checkPciDevice(PciBus *bus, uint32_t slot) {
	uint16_t vendor = readPciHalf(bus->busId, slot, 0, kPciVendor);
	if(vendor == 0xFFFF)
		return;

	frigg::infoLogger() << "    Bus: " << bus->busId << ", slot " << slot << frigg::endLog;

	uint8_t header_type = readPciByte(bus->busId, slot, 0, kPciHeaderType);
	if((header_type & 0x80) != 0) {
		for(uint32_t function = 0; function < 8; function++)
			checkPciFunction(bus, slot, function);
	}else{
		checkPciFunction(bus, slot, 0);
	}
}

void checkPciBus(PciBus *bus) {
	for(uint32_t slot = 0; slot < 32; slot++)
		checkPciDevice(bus, slot);
}

void runAllDevices() {
	for(auto it = allDevices->begin(); it != allDevices->end(); ++it) {
		runDevice(*it);
	}
}

void enumerateSystemBusses() {
	enumerationQueue.initialize(*kernelAlloc);
	allDevices.initialize(*kernelAlloc);

	LAI_CLEANUP_STATE lai_state_t laiState;
	lai_init_state(&laiState);

	LAI_CLEANUP_VAR lai_variable_t pci_pnp_id = LAI_VAR_INITIALIZER;
	LAI_CLEANUP_VAR lai_variable_t pcie_pnp_id = LAI_VAR_INITIALIZER;
	lai_eisaid(&pci_pnp_id, "PNP0A03");
	lai_eisaid(&pcie_pnp_id, "PNP0A08");

	lai_nsnode_t *sb_handle = lai_resolve_path(NULL, "\\_SB_");
	LAI_ENSURE(sb_handle);
	struct lai_ns_child_iterator iter = LAI_NS_CHILD_ITERATOR_INITIALIZER(sb_handle);
	lai_nsnode_t *handle;
	while ((handle = lai_ns_child_iterate(&iter))) {
		if (lai_check_device_pnp_id(handle, &pci_pnp_id, &laiState)
				&& lai_check_device_pnp_id(handle, &pcie_pnp_id, &laiState))
			continue;

		frigg::infoLogger() << "thor: Found PCI host bridge" << frigg::endLog;
		auto rootBus = frg::construct<PciBus>(*kernelAlloc, nullptr, 0, handle);
		enumerationQueue->push_back(std::move(rootBus)); // FIXME: the move should not be necessary.
	}

	// Note that elements are added to this queue while it is being traversed.
	frigg::infoLogger() << "thor: Discovering PCI devices" << frigg::endLog;
	for(size_t i = 0; i < enumerationQueue->size(); i++) {
		auto bus = (*enumerationQueue)[i];
		checkPciBus(bus);
	}
}

} } // namespace thor::pci
