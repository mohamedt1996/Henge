; Vertex: positions from a device buffer (driver table), scale from a constant buffer, one varying.
%VOut = type { <4 x float>, <2 x float> }

define %VOut @vs_quad(i32 %vid, ptr addrspace(1) noalias %pos, ptr addrspace(2) noalias %xf) {
entry:
  %i = zext i32 %vid to i64
  %p = getelementptr inbounds <2 x float>, ptr addrspace(1) %pos, i64 %i
  %v = load <2 x float>, ptr addrspace(1) %p, align 8
  %s = load <2 x float>, ptr addrspace(2) %xf, align 8
  %m = fmul <2 x float> %v, %s
  %x = extractelement <2 x float> %m, i32 0
  %y = extractelement <2 x float> %m, i32 1
  %p0 = insertelement <4 x float> <float 0.0, float 0.0, float 0.0, float 1.0>, float %x, i32 0
  %p1 = insertelement <4 x float> %p0, float %y, i32 1
  %uv = fmul <2 x float> %v, <float 0.5, float 0.5>
  %uv1 = fadd <2 x float> %uv, <float 0.5, float 0.5>
  %r0 = insertvalue %VOut undef, <4 x float> %p1, 0
  %r1 = insertvalue %VOut %r0, <2 x float> %uv1, 1
  ret %VOut %r1
}

!air.vertex = !{!0}
!0 = !{ptr @vs_quad, !1, !2}
!1 = !{!3, !4}
!3 = !{!"air.position", !"air.arg_type_name", !"float4", !"air.arg_name", !"position"}
!4 = !{!"air.vertex_output", !"user(locn0)", !"air.arg_type_name", !"float2", !"air.arg_name", !"uv"}
!2 = !{!5, !6, !7}
!5 = !{i32 0, !"air.vertex_id", !"air.arg_type_name", !"uint", !"air.arg_name", !"vid"}
!6 = !{i32 1, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 8, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"float2", !"air.arg_name", !"pos"}
!7 = !{i32 2, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 8, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"float2", !"air.arg_name", !"xf"}
