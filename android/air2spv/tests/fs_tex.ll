; Fragment: sample texture(0) with sampler(0), multiply by a constant-buffer tint.
declare { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1), ptr addrspace(2), <2 x float>, i1, <2 x i32>, i1, float, float, i32)

define <4 x float> @fs_tex(<4 x float> %pos, <2 x float> %uv, ptr addrspace(1) %tex, ptr addrspace(2) %smp, ptr addrspace(2) %tint) {
entry:
  %s = call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) %tex, ptr addrspace(2) %smp, <2 x float> %uv, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.0, float 0.0, i32 0)
  %c = extractvalue { <4 x float>, i8 } %s, 0
  %t = load <4 x float>, ptr addrspace(2) %tint, align 16
  %r = fmul <4 x float> %c, %t
  ret <4 x float> %r
}

!air.fragment = !{!0}
!0 = !{ptr @fs_tex, !1, !2}
!1 = !{!3}
!3 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"color"}
!2 = !{!4, !5, !6, !7, !8}
!4 = !{i32 0, !"air.position", !"air.center", !"air.no_perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"pos"}
!5 = !{i32 1, !"air.fragment_input", !"user(locn0)", !"air.center", !"air.perspective", !"air.arg_type_name", !"float2", !"air.arg_name", !"uv"}
!6 = !{i32 2, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"tex"}
!7 = !{i32 3, !"air.sampler", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"sampler", !"air.arg_name", !"smp"}
!8 = !{i32 4, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"float4", !"air.arg_name", !"tint"}
