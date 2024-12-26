set_project("Uinxed")
add_rules("mode.debug", "mode.release")

set_arch("i386")
set_languages("c23")
set_optimize("fastest")
set_warnings("all", "extra")
set_policy("run.autobuild", true)
set_policy("check.auto_ignore_flags", false)

add_cflags("-Wno-unused-parameter")
add_cflags("-mno-mmx", "-mno-sse", "-mno-sse2")
add_cflags("-nostdinc", "-fno-pic", "-fno-builtin", "-fno-stack-protector")
add_ldflags("-nostdlib", "-static")

target("kernel")
    set_kind("binary")
    set_toolchains("gcc", "nasm")
    set_toolset("as", "nasm")
    set_default(false)

    add_linkdirs("lib")
    add_includedirs("include")

    add_links("klogo")
    add_links("elf_parse")
    add_links("os_terminal")
    add_links("pl_readline")
    add_files("**/*.s", "**/*.c")

    add_asflags("-f", "elf32")
    add_ldflags("-T", "scripts/kernel.ld")

target("iso")
    set_kind("phony")
    add_deps("kernel")
    set_default(true)

    on_build(function (target)
        import("core.project.project")
        local iso_dir = "$(buildir)/iso_dir"

        if os.exists(iso_dir) then os.rmdir(iso_dir) end
        os.cp("assets", iso_dir)

        local kernel = project.target("kernel")
        os.cp(kernel:targetfile(), iso_dir .. "/UxImage")

        local iso_file = "$(buildir)/Uinxed.iso"
        local xorriso_flags = "-b limine-bios-cd.bin -no-emul-boot -boot-info-table"
        os.run("xorriso -as mkisofs %s %s -o %s", xorriso_flags, iso_dir, iso_file)
        print("ISO image created at: " .. iso_file)
    end)

    on_run(function (target)
        local misc = "-serial stdio"
        local speaker = " -audiodev pa,id=speaker -machine pcspk-audiodev=speaker "
        local kvm = " -enable-kvm"
        local flags = misc..speaker..kvm

        os.exec("qemu-system-i386 -cdrom $(buildir)/Uinxed.iso %s", flags)
    end)
