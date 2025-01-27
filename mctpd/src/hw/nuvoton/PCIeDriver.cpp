#include "hw/nuvoton/PCIeDriver.hpp"

namespace hw
{
namespace nuvoton
{

PCIeDriver::PCIeDriver(boost::asio::io_context& ioc) : streamMonitor(ioc)
{
}

void PCIeDriver::init()
{
    pcie = mctp_nupcie_init();
    if (pcie == nullptr)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Error in MCTP PCIe init");
        throw std::system_error(
            std::make_error_code(std::errc::not_enough_memory));
    }
}

mctp_binding* PCIeDriver::binding()
{
    return mctp_nupcie_core(pcie);
}

void PCIeDriver::pollRx()
{
    if (!streamMonitor.is_open())
    {
        // Can't be assigned in 'init()', as it needs to be performed after
        // bus registration
        streamMonitor.assign(mctp_nupcie_get_fd(pcie));
    }

    streamMonitor.async_wait(
        boost::asio::posix::stream_descriptor::wait_read,
        [this](const boost::system::error_code& ec) {
            if (ec)
            {
                phosphor::logging::log<phosphor::logging::level::ERR>(
                    "Error reading PCIe response");
                pollRx();
            }
            mctp_nupcie_rx(pcie);
            pollRx();
        });
}

bool PCIeDriver::registerAsDefault()
{
    //nu todo
    //return !mctp_nupcie_register_default_handler(pcie);
    return true;
}

//nu todo
bool PCIeDriver::getBdf(uint16_t& bdf)
{
    //if (mctp_nupcie_get_bdf(pcie, &bdf) != 0)
    //{
    //    phosphor::logging::log<phosphor::logging::level::ERR>(
    //        "Nupcie get bdf failed");
    //    return false;
    //}
    memset(&bdf, 0, sizeof(bdf));
    return true;
}
uint8_t PCIeDriver::getMediumId()
{
    //nu todo
    //return mctp_nupcie_get_medium_id(pcie);
    return 0;
}
//nu todo
bool PCIeDriver::setEndpointMap(std::vector<EidInfo>& endpoints)
{
    //nu todo
    //return !mctp_nupcie_set_eid_info_ioctl(
    //    pcie, endpoints.data(), static_cast<uint16_t>(endpoints.size()));
    if(0 == endpoints.size()) {
       return false;
    }
    return true;
}
PCIeDriver::~PCIeDriver()
{
    streamMonitor.release();

    if (pcie)
    {
        mctp_nupcie_free(pcie);
    }
}

} // namespace nuvoton
} // namespace hw
