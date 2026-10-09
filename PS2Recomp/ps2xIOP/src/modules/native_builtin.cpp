// Native stand-ins for IOP modules whose EE-facing libraries are already implemented natively
// by the runtime (scePad*, sceMc*, sceSd* stubs) so the physical IRX never needs to execute.
// Claiming them here keeps the R3000 emulator idle; any RPC that still reaches one of these
// modules is logged so a missing native path is visible rather than silently emulated.
#include "module_factories.h"

#include <array>
#include <sstream>

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr std::array<std::string_view, 6> kAliases{"sio2man", "padman", "mcman", "mcserv", "libsd", "sdrdrv"};

        class NativeBuiltinModulesService final : public IopService
        {
        public:
            explicit NativeBuiltinModulesService(IopHost &host) : m_host(host) {}

            [[nodiscard]] std::string_view name() const override { return "native-builtin"; }
            [[nodiscard]] std::span<const uint32_t> sids() const override { return {}; }
            [[nodiscard]] std::span<const std::string_view> moduleAliases() const override { return kAliases; }
            [[nodiscard]] bool replacesPhysicalModule() const override { return true; }
            void reset() override {}

            [[nodiscard]] RpcResult handleRpc(const RpcRequest &request) override
            {
                std::ostringstream message;
                message << "[IOP:native-builtin] unexpected RPC sid=0x" << std::hex << request.sid << " fn=0x" << request.function;
                m_host.log(LogLevel::Warning, message.str());
                return {};
            }

        private:
            IopHost &m_host;
        };
    }

    std::unique_ptr<IopService> createNativeBuiltinModulesService(IopHost &host)
    {
        return std::make_unique<NativeBuiltinModulesService>(host);
    }
}
