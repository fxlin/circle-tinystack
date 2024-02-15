#!/usr/bin/env python3

import errno, optparse, os, select, subprocess, sys, time, zlib
import operator
import re
import argparse


keyword="num"

'''
num=00000064 ts=00.779934 evid=01 para0=3F980014 para1=05000029 para2=00000000 para3=00000000

1. count
2. ts (sec
3. event 01=r 02=w
4. addr
5. val 
6. unused
7. unused
'''
lineregex_access_reg=r'''num=(\d+) ts=(\d+\.\d+) evid=(\d+) para0=(\S+) para1=(\S+) para2=(\S+) para3=(\S+)'''

# tracer.h
event_names = {
    1: 'reg read',
    2: 'reg write', 
    3: 'irq start >>>>>',
    4: 'irq end <<<<<<',
    5: 'sofirq start >>>>>',
    6: 'sofirq end <<<<<<',
    7: 'write start >>>>>',
    8: 'write end <<<<<<',
    9: 'read start >>>>>',
    10: 'read end <<<<<<',                
}

arm_io_base = 0x3F000000 
core_base_addr = arm_io_base + 0x980000
host_base_addr = core_base_addr + 0x400

# dwhci.h
core_regnames = {
    0:'DWHCI_CORE_OTG_CTRL',
    0x4:'DWHCI_CORE_OTG_INT',
    0x8:'DWHCI_CORE_AHB_CFG',
    0xc:'DWHCI_CORE_USB_CFG',
    0x10:'DWHCI_CORE_RESET',
    0x14:'DWHCI_CORE_INT_STAT',
    0x18:'DWHCI_CORE_INT_MASK',
    0x1c:'DWHCI_CORE_RX_STAT_RD',
    0x20:'DWHCI_CORE_RX_STAT_POP',
    0x24:'DWHCI_CORE_RX_FIFO_SIZ',
    0x28:'DWHCI_CORE_NPER_TX_FIFO_SIZ',
    0x2c: 'DWHCI_CORE_NPER_TX_STAT',
    0x30: 'DWHCI_CORE_I2C_CTRL',
    0x34: 'DWHCI_CORE_PHY_VENDOR_CTRL',
    0x38: 'DWHCI_CORE_GPIO',
    0x3c: 'DWHCI_CORE_USER_ID',
    0x40: 'DWHCI_CORE_VENDOR_ID',
    0x44: 'DWHCI_CORE_HW_CFG1',
    0x48:'DWHCI_CORE_HW_CFG2',
    0x4c:'DWHCI_CORE_HW_CFG3',
    0x50:'DWHCI_CORE_HW_CFG4',
    0x54:'DWHCI_CORE_LPM_CFG',
    0x58: 'DWHCI_CORE_POWER_DOWN',
    0x5c: 'DWHCI_CORE_DFIFO_CFG',
    0x60: 'DWHCI_CORE_ADP_CTRL',
    # gap
    0x80: 'DWHCI_VENDOR_MDIO_CTRL',
    0x84: 'DWHCI_VENDOR_MDIO_DATA',
    0x88: 'DWHCI_VENDOR_VBUS_DRV',
    # gap
    0x100: 'DWHCI_CORE_HOST_PER_TX_FIFO_SIZ',
    # more ... channels...TBD    
}

host_regnames = {
    0:'DWHCI_HOST_CFG',
    0x4:'DWHCI_HOST_FRM_INTERVAL',
    0x8:'DWHCI_HOST_FRM_NUM',
    # gap
    0x10:'DWHCI_HOST_PER_TX_FIFO_STAT',
    0x14:'DWHCI_HOST_ALLCHAN_INT',
    0x18:'DWHCI_HOST_ALLCHAN_INT_MASK',
    0x1c:'DWHCI_HOST_FRMLST_BASE',
    # gap
    0x40: 'DWHCI_HOST_PORT',
    # more channels... TBD
    0x100 + 0 * 0x20: 'DWHCI_HOST_CHAN_CHARACTER(0)',
    0x100 + 1 * 0x20: 'DWHCI_HOST_CHAN_CHARACTER(1)',
    0x100 + 2 * 0x20: 'DWHCI_HOST_CHAN_CHARACTER(2)',
    0x100 + 3 * 0x20: 'DWHCI_HOST_CHAN_CHARACTER(3)',
    0x100 + 4 * 0x20: 'DWHCI_HOST_CHAN_CHARACTER(4)',
    0x100 + 5 * 0x20: 'DWHCI_HOST_CHAN_CHARACTER(5)',
    0x100 + 6 * 0x20: 'DWHCI_HOST_CHAN_CHARACTER(6)',
    # split
    0x104 + 0 * 0x20: 'DWHCI_HOST_CHAN_SPLIT_CTRL(0)',
    0x104 + 1 * 0x20: 'DWHCI_HOST_CHAN_SPLIT_CTRL(1)',
    0x104 + 2 * 0x20: 'DWHCI_HOST_CHAN_SPLIT_CTRL(2)',
    0x104 + 3 * 0x20: 'DWHCI_HOST_CHAN_SPLIT_CTRL(3)',
    0x104 + 4 * 0x20: 'DWHCI_HOST_CHAN_SPLIT_CTRL(4)',
    # int
    0x108 + 0 * 0x20: 'DWHCI_HOST_CHAN_INT(0)',
    0x108 + 1 * 0x20: 'DWHCI_HOST_CHAN_INT(1)',
    0x108 + 2 * 0x20: 'DWHCI_HOST_CHAN_INT(2)',
    0x108 + 3 * 0x20: 'DWHCI_HOST_CHAN_INT(3)',
    0x108 + 4 * 0x20: 'DWHCI_HOST_CHAN_INT(4)',
    # int mask
    0x10c + 0 * 0x20: 'DWHCI_HOST_CHAN_INT_MASK(0)',
    0x10c + 1 * 0x20: 'DWHCI_HOST_CHAN_INT_MASK(1)',
    0x10c + 2 * 0x20: 'DWHCI_HOST_CHAN_INT_MASK(2)',
    0x10c + 3 * 0x20: 'DWHCI_HOST_CHAN_INT_MASK(3)',
    0x10c + 4 * 0x20: 'DWHCI_HOST_CHAN_INT_MASK(4)',
    # xfer size 
    0x110 + 0 * 0x20: 'DWHCI_HOST_CHAN_XFER_SIZ(0)',
    0x110 + 1 * 0x20: 'DWHCI_HOST_CHAN_XFER_SIZ(1)',
    0x110 + 2 * 0x20: 'DWHCI_HOST_CHAN_XFER_SIZ(2)',
    0x110 + 3 * 0x20: 'DWHCI_HOST_CHAN_XFER_SIZ(3)',
    0x110 + 4 * 0x20: 'DWHCI_HOST_CHAN_XFER_SIZ(4)',    
    # dma addr
    0x114 + 0 * 0x20: 'DWHCI_HOST_CHAN_DMA_ADDR(0)',
    0x114 + 1 * 0x20: 'DWHCI_HOST_CHAN_DMA_ADDR(1)',
    0x114 + 2 * 0x20: 'DWHCI_HOST_CHAN_DMA_ADDR(2)',
    0x114 + 3 * 0x20: 'DWHCI_HOST_CHAN_DMA_ADDR(3)',
    0x114 + 4 * 0x20: 'DWHCI_HOST_CHAN_DMA_ADDR(4)',
        
    0xe00-0x400: 'ARM_USB_POWER'
}

xfer_pid = {
    0: "data0",
    1: "data2",
    2: "data1",
    3: "setup/mdata"
}

'''
return match, else None
'''
def parseline(line):
    global emitcounter
    global the_records
    global the_gpu_state 
    global the_cache_flush, the_mmu_flush
    global total_irq_delay
    global the_max_pages, the_current_pages
    
    comment=""
    
    # reg access    
    m = re.match(lineregex_access_reg, line)
    if m: 
        ev_num = int(m.group(3))
        addr_num=int(m.group(4),16)
        val_num = int(m.group(5), 16)
        
        ev = "(??)"
        regname = ""        
        prefix = ""
        
        if ev_num in event_names:
            ev = event_names[ev_num]
        
        if addr_num < host_base_addr: 
            offset = addr_num - core_base_addr
            if offset in core_regnames: 
                regname = core_regnames[offset]
            else: 
                regname = "reg(??)"         
        else: 
            offset = addr_num - host_base_addr
            if offset in host_regnames: 
                regname = host_regnames[offset]
            else: 
                regname = "reg(??)"
            
        #comment += regname        
        
        if regname.startswith("DWHCI_HOST_CHAN_XFER_SIZ"):
            comment += f'''bytes: {val_num & 0x7ffff} packets:{(val_num>>19)&0x3ff} pid:{(val_num>>29)&3} {xfer_pid[(val_num>>29)&3]}'''
            
        print(f'''{prefix}{{.entry_access_reg = {{ {m.group(3)} /*{ev}*/, {m.group(4)}/*{regname}*/, {m.group(5)}/*val*/ }}, "{comment}" }},''')
    
    return m 

once_in_trace = True     # first entry in the trace; may be out of window
once = True

begin_ts = 0
end_ts = 0
last_ts = 0
last_checked_ts = 0 # although we may skipped this (e.g, out of window)
last_gpu_state = "idle"
last_suspend = 0
last_resume = 0
last_resume_fix = -1    # resume, after clock rollback
total_long_delay = 0.0
total_long_delay_idle = 0.0
total_irq_delay = 0.0    # in sec. we only have at most 1 irq outstanding, so this is meaningful 


if __name__ == '__main__':
    
    parser = argparse.ArgumentParser()
    parser.add_argument("-j", "--json", 
                    help='output json (default: output c header)', 
                    action="store_true")    
    parser.add_argument("input", help="input file")
    parser.add_argument("-v", "--varname", help="name for the c array")
    
    args = parser.parse_args()
    if args.json:
        out_c_header = 0    
            
    f=open(args.input)
    lines=f.readlines()
        
    nlines = len(lines) 
    
    i = 0
    badlines=0
        
    emitcounter = 0
    while i < nlines:
        line = lines[i]
        
        # we must be careful as comm (process name) may contain spaces
        # we are parsing the body 
        i += 1
        
        if line.find(keyword) == -1:
            continue
                 
        long_delay = 0.0
        # process the line's timestamp, before processing the line itself            
        res = re.match(lineregex_access_reg, line)
        if res:
            ts = float(res.group(2))
            
            if once_in_trace:
                begin_ts = ts
                once_in_trace = False
            else:
                if ts - last_checked_ts > 0.05: # 50 ms 
                    long_delay = ts - last_checked_ts
                    #print(f"/* {print_gpu_state()} long delay = {long_delay} */")                    
                    print(f"/* long delay = {long_delay} */")
                    total_long_delay += long_delay 
            last_checked_ts = ts
        else: # malformed line?
            print(line, "bug?")
            sys.exit(-1)
                            
        #prev_gpu_state = the_gpu_state
        
        res = parseline(line)
        if res == None: # cannot parse, emit as a comment line
            print("/*", line.replace('\n',''), "*/")
            badlines += 1
            
