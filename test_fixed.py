import struct                                                                 

HDF = "/home/john/Documents/Code/AmigaPart/DiagDisk.img"                      
                
def read_block(f, n, bsz=512):                                                
    f.seek(n * bsz)
    return struct.unpack_from(f'>{bsz//4}I', f.read(bsz))                     
                                                                              
with open(HDF, 'rb') as f:                                                    
    # Find RDSK                                                               
    rdsk = None                                                               
    for b in range(16):
        blk = read_block(f, b)                                                
        if blk[0] == 0x5244534B:  # RDSK                                      
            rdsk = blk; rdsk_blk = b; break
    if not rdsk:                                                              
        print("No RDSK found"); exit(1)
                                                                              
    print(f"RDSK at block {rdsk_blk}")
    part_list = rdsk[7]                                                       
                                                                              
    # Walk PART chain
    next_part = part_list                                                     
    while next_part not in (0, 0xFFFFFFFF):
        blk = read_block(f, next_part)                                        
        if blk[0] != 0x50415254:  # PART
            break                                                             
        # DosEnvec starts at long[32] of the PART block
        env = blk[32:]                                                        
        lo_cyl   = env[9]
        hi_cyl   = env[10]                                                    
        heads    = env[3]
        secs     = env[5]                                                     
        dostype  = env[16]
        # Drive name is a BSTR at bytes 36-67 (longs 9-16 of PART header)     
        name_bytes = bytes(struct.pack(f'>{"I"*8}', *blk[9:17]))              
        name_len = name_bytes[0]                                              
        name = name_bytes[1:1+name_len].decode('ascii', errors='?')           
                                                                              
        num_blocks = (hi_cyl - lo_cyl + 1) * heads * secs                     
        root_rel   = num_blocks // 2                                          
        part_abs   = lo_cyl * heads * secs                                    
        root_abs   = part_abs + root_rel
                                                                              
        print(f"\nPART '{name}' at block {next_part}")
        print(f"  LoCyl={lo_cyl} HiCyl={hi_cyl} Heads={heads} Secs={secs}")   
        print(f"  DosType=0x{dostype:08X}  num_blocks={num_blocks}")          
        print(f"  Expected root: rel={root_rel} abs={root_abs}")              
                                                                              
        # Read the expected root block                                        
        rb = read_block(f, root_abs)                                          
        cs_check = sum(rb) & 0xFFFFFFFF
        print(f"\n  Root block at abs {root_abs}:")                           
        print(f"    L[0] type     = 0x{rb[0]:X}  (want 0x2 T_SHORT)")         
        print(f"    L[1] own_key  = {rb[1]}  (expect {root_rel})")            
        print(f"    L[4] disk_sz  = {rb[4]}  (expect {num_blocks})")          
        print(f"    L[5] checksum = 0x{rb[5]:08X}  sum_all={'OK' if           
cs_check==0 else f'FAIL {hex(cs_check)}'}")                                   
        print(f"    L[78] bm_flag = 0x{rb[78]:X}  (want 0xFFFFFFFF)")         
        print(f"    L[79] bm[0]   = {rb[79]}")                                
        print(f"    L[104] bm_ext = {rb[104]}")
        print(f"    L[127] sec_type = 0x{rb[127]:X}  (want 0x1 ST_ROOT)")     
                                                                              
        # Also read old root (old num_blocks/2) if different                  
        # We'd need to know old geometry for that                             
                                                                              
        next_part = blk[4]  # pb_Next   
