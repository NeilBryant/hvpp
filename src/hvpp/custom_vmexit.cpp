#include "custom_vmexit.h"

#include "lib/cr3_guard.h"
#include "lib/mp.h"
#include "lib/log.h"

void custom_vmexit_handler::setup(vcpu_t& vp) noexcept
{
  vmexit_base_handler::setup(vp);

#if 0
  //
  // Turn on VM-exit on everything we support.
  //

  auto procbased_ctls = vp.processor_based_controls();

  //
  // Since VMWare handles rdtsc(p) instructions by its own magical way, we'll
  // disable our own handling. Setting this in VMWare makes the guest OS
  // completely bananas.
  //
  // procbased_ctls.rdtsc_exiting = true;

  //
  // Use either "use_io_bitmaps" or "unconditional_io_exiting", try to avoid
  // using both of them.
  //

  // procbased_ctls.use_io_bitmaps = true;
  procbased_ctls.unconditional_io_exiting = true;
  procbased_ctls.mov_dr_exiting = true;
  procbased_ctls.cr3_load_exiting = true;
  procbased_ctls.cr3_store_exiting = true;
  vp.processor_based_controls(procbased_ctls);

  auto procbased_ctls2 = vp.processor_based_controls2();
  procbased_ctls2.descriptor_table_exiting = true;
  vp.processor_based_controls2(procbased_ctls2);

  vmx::msr_bitmap_t msr_bitmap{ 0 };
  memset(msr_bitmap.data, 0xff, sizeof(msr_bitmap));
  vp.msr_bitmap(msr_bitmap);

  if (procbased_ctls.use_io_bitmaps)
  {
    vmx::io_bitmap_t io_bitmap{ 0 };
    memset(io_bitmap.data, 0xff, sizeof(io_bitmap));
    vp.io_bitmap(io_bitmap);
  }

  //
  // Catch all exceptions.
  //
  vp.exception_bitmap(vmx::exception_bitmap_t{ ~0ul });

  //
  // VM-execution control fields include guest/host masks and read shadows for the CR0 and CR4 registers. These
  // fields control executions of instructions that access those registers (including CLTS, LMSW, MOV CR, and SMSW).
  // They are 64 bits on processors that support Intel 64 architecture and 32 bits on processors that do not.
  // In general, bits set to 1 in a guest/host mask correspond to bits "owned" by the host:
  //   - Guest attempts to set them (using CLTS, LMSW, or MOV to CR) to values differing from the corresponding bits
  //     in the corresponding read shadow cause VM exits.
  //   - Guest reads (using MOV from CR or SMSW) return values for these bits from the corresponding read shadow.
  // Bits cleared to 0 correspond to bits "owned" by the guest; guest attempts to modify them succeed and guest reads
  // return values for these bits from the control register itself.
  // (ref: Vol3C[24.6.6(Guest/Host Masks and Read Shadows for CR0 and CR4)])
  //
  // TL;DR:
  //   When bit in guest/host mask is set, write to the control register causes VM-exit.
  //   Mov FROM CR0 and CR4 returns values in the shadow register values.
  //
  // Note that SHADOW register value and REAL register value may differ. The guest will behave according
  // to the REAL control register value. Only read from that register will return the fake (aka "shadow")
  // value.
  //

  vp.cr0_guest_host_mask(cr0_t{ ~0ull });
  vp.cr4_guest_host_mask(cr4_t{ ~0ull });
#endif
}

void custom_vmexit_handler::handle_execute_cpuid(vcpu_t& vp) noexcept
{
  if (vp.exit_context().eax == 'ppvh')
  {
    //
    // "hello from hvpp\0"
    //
    vp.exit_context().rax = 'lleh';
    vp.exit_context().rbx = 'rf o';
    vp.exit_context().rcx = 'h mo';
    vp.exit_context().rdx = 'ppv';
  }
  else
  {
    vmexit_base_handler::handle_execute_cpuid(vp);
  }
}

void custom_vmexit_handler::handle_execute_vmcall(vcpu_t& vp) noexcept
{
  auto& data = data_[mp::cpu_index()];

  switch (vp.exit_context().rcx)
  {
    case 0xc1:
      {
        cr3_guard _(vp.guest_cr3());

        data.page_read = pa_t::from_va(vp.exit_context().rdx_as_pointer);
        data.page_exec = pa_t::from_va(vp.exit_context().r8_as_pointer);
      }

      hvpp_trace("vmcall (hook) EXEC: 0x%p READ: 0x%p", data.page_exec.value(), data.page_read.value());

      //
      // Set execute-only access.
      //
      vp.ept().map_4kb(data.page_exec, data.page_exec, epte_t::access_type::execute);
      break;

    case 0xc2:
      hvpp_trace("vmcall (unhook)");

      //
      // Set back read-write-execute access.
      //
      vp.ept().map_4kb(data.page_exec, data.page_exec, epte_t::access_type::read_write_execute);
      break;

    default:
      vmexit_base_handler::handle_execute_vmcall(vp);
      return;
  }

  vmx::invept(vmx::invept_t::all_context);
  vmx::invvpid(vmx::invvpid_t::all_context);
}

void custom_vmexit_handler::handle_ept_violation(vcpu_t& vp) noexcept
{
  auto exit_qualification = vp.exit_qualification().ept_violation;
  auto guest_pa = vp.exit_guest_physical_address();
  auto guest_la = vp.exit_guest_linear_address();

  auto& data = data_[mp::cpu_index()];

  if (exit_qualification.data_read || exit_qualification.data_write)
  {
    //
    // Someone requested read or write access to the guest_pa, but the page
    // has execute-only access.
    // Map the page with "data.page_read" we've saved before in VMCALL handler
    // and set the access to RW.
    //
    hvpp_trace("data_read LA: 0x%p PA: 0x%p", guest_la, guest_pa.value());

    vp.ept().map_4kb(data.page_exec, data.page_read, epte_t::access_type::read_write);
  }
  else if (exit_qualification.data_execute)
  {
    //
    // Someone requested execute access to the guest_pa, but the page has only
    // read-write access.
    // Map the page with "data.page_execute" we've saved before in VMCALL handler
    // and set the access to execute-only.
    //
    hvpp_trace("data_execute LA: 0x%p PA: 0x%p", guest_la, guest_pa.value());

    vp.ept().map_4kb(data.page_exec, data.page_exec, epte_t::access_type::execute);
  }

  vmx::invept(vmx::invept_t::all_context);
  vmx::invvpid(vmx::invvpid_t::all_context);

  //
  // Make the instruction which fetched the memory to be executed again (this
  // time without EPT violation).
  //
  vp.suppress_rip_adjust();
}
