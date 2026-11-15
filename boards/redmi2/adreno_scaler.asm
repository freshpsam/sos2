; Fixed 480x800 -> 720x1280 nearest-neighbor display kernels.
; c5: source IOVA, output IOVA, output stride, bytes/pixel.
; c6: 21846, 0xf800, 0x7e0, 0xff000000.

; xrgb8888
(sy)(rpt4)nop
(sy)(ss)add.u r1.x, c2.y, r0.x
add.u r1.y, c2.z, r0.y
(rpt2)nop
mul.u r2.x, r1.x, c6.x
mul.u r2.y, r1.y, 5
(rpt2)nop
shr.b r2.x, r2.x, 15
shr.b r2.y, r2.y, 3
(rpt2)nop
mul.u r2.y, r2.y, 480
(rpt2)nop
add.u r2.x, r2.x, r2.y
(rpt2)nop
shl.b r2.x, r2.x, 2
(rpt2)nop
add.u r2.x, r2.x, c5.x
(rpt5)nop
ldg.u32 r3.x, g[r2.x], 1
mul.u r2.z, r1.y, c5.z
mul.u r2.w, r1.x, c5.w
(rpt2)nop
add.u r2.z, r2.z, r2.w
(rpt2)nop
add.u r2.z, r2.z, c5.y
(rpt5)nop
(sy)stg.u32 g[r2.z], r3.x, 1
end

; rgb565
(sy)(rpt4)nop
(sy)(ss)add.u r1.x, c2.y, r0.x
add.u r1.y, c2.z, r0.y
(rpt2)nop
mul.u r2.x, r1.x, c6.x
mul.u r2.y, r1.y, 5
(rpt2)nop
shr.b r2.x, r2.x, 15
shr.b r2.y, r2.y, 3
(rpt2)nop
mul.u r2.y, r2.y, 480
(rpt2)nop
add.u r2.x, r2.x, r2.y
(rpt2)nop
shl.b r2.x, r2.x, 2
(rpt2)nop
add.u r2.x, r2.x, c5.x
(rpt5)nop
ldg.u32 r3.x, g[r2.x], 1
mul.u r2.z, r1.y, c5.z
mul.u r2.w, r1.x, c5.w
(rpt2)nop
add.u r2.z, r2.z, r2.w
(rpt2)nop
add.u r2.z, r2.z, c5.y
(rpt5)nop
(sy)shr.b r3.y, r3.x, 8
shr.b r3.z, r3.x, 5
shr.b r3.x, r3.x, 3
(rpt2)nop
and.b r3.y, r3.y, c6.y
and.b r3.z, r3.z, c6.z
and.b r3.x, r3.x, 31
(rpt2)nop
or.b r3.x, r3.x, r3.y
(rpt2)nop
or.b r3.x, r3.x, r3.z
(rpt5)nop
stg.u16 g[r2.z], r3.x, 1
end

; rgb888
(sy)(rpt4)nop
(sy)(ss)add.u r1.x, c2.y, r0.x
add.u r1.y, c2.z, r0.y
(rpt2)nop
mul.u r2.x, r1.x, c6.x
mul.u r2.y, r1.y, 5
(rpt2)nop
shr.b r2.x, r2.x, 15
shr.b r2.y, r2.y, 3
(rpt2)nop
mul.u r2.y, r2.y, 480
(rpt2)nop
add.u r2.x, r2.x, r2.y
(rpt2)nop
shl.b r2.x, r2.x, 2
(rpt2)nop
add.u r2.x, r2.x, c5.x
(rpt5)nop
ldg.u32 r3.x, g[r2.x], 1
mul.u r2.z, r1.y, c5.z
mul.u r2.w, r1.x, c5.w
(rpt2)nop
add.u r2.z, r2.z, r2.w
(rpt2)nop
add.u r2.z, r2.z, c5.y
(rpt5)nop
(sy)shr.b r3.y, r3.x, 8
shr.b r3.z, r3.x, 16
(rpt5)nop
stg.u8 g[r2.z], r3.x, 3
end
(rpt0)nop

; argb8888
(sy)(rpt4)nop
(sy)(ss)add.u r1.x, c2.y, r0.x
add.u r1.y, c2.z, r0.y
(rpt2)nop
mul.u r2.x, r1.x, c6.x
mul.u r2.y, r1.y, 5
(rpt2)nop
shr.b r2.x, r2.x, 15
shr.b r2.y, r2.y, 3
(rpt2)nop
mul.u r2.y, r2.y, 480
(rpt2)nop
add.u r2.x, r2.x, r2.y
(rpt2)nop
shl.b r2.x, r2.x, 2
(rpt2)nop
add.u r2.x, r2.x, c5.x
(rpt5)nop
ldg.u32 r3.x, g[r2.x], 1
mul.u r2.z, r1.y, c5.z
mul.u r2.w, r1.x, c5.w
(rpt2)nop
add.u r2.z, r2.z, r2.w
(rpt2)nop
add.u r2.z, r2.z, c5.y
(rpt5)nop
(sy)or.b r3.x, r3.x, c6.w
(rpt5)nop
stg.u32 g[r2.z], r3.x, 1
end
(rpt0)nop
(rpt0)nop

; copy
(sy)(rpt4)nop
(sy)(ss)add.u r1.x, c2.y, r0.x
add.u r1.y, c2.z, r0.y
(rpt2)nop
mul.u r2.x, r1.y, c5.z
shl.b r2.y, r1.x, 2
(rpt2)nop
add.u r2.x, r2.x, r2.y
(rpt2)nop
add.u r2.z, r2.x, c5.y
add.u r2.x, r2.x, c5.x
(rpt5)nop
ldg.u32 r3.x, g[r2.x], 1
(sy)(rpt5)nop
(sy)stg.u32 g[r2.z], r3.x, 1
end
