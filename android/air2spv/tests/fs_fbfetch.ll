; Fragment: read the destination colour (framebuffer fetch) and darken it; uses a constant table.
@lut = internal addrspace(2) constant [4 x float] [float 0.25, float 0.5, float 0.75, float 1.0], align 4

define <4 x float> @fs_fbfetch(<4 x float> %pos, <4 x float> %dst) {
entry:
  %fx = extractelement <4 x float> %pos, i32 0
  %ix = fptoui float %fx to i32
  %k = and i32 %ix, 3
  %k64 = zext i32 %k to i64
  %p = getelementptr inbounds [4 x float], ptr addrspace(2) @lut, i64 0, i64 %k64
  %f = load float, ptr addrspace(2) %p, align 4
  %fs = insertelement <4 x float> undef, float %f, i32 0
  %fv = shufflevector <4 x float> %fs, <4 x float> undef, <4 x i32> zeroinitializer
  %r = fmul <4 x float> %dst, %fv
  ret <4 x float> %r
}

!air.fragment = !{!0}
!0 = !{ptr @fs_fbfetch, !1, !2}
!1 = !{!3}
!3 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"color"}
!2 = !{!4, !5}
!4 = !{i32 0, !"air.position", !"air.center", !"air.no_perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"pos"}
!5 = !{i32 1, !"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"dst"}
