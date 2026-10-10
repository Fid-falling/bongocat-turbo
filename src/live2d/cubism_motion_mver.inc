/*
 * 配置阶段由 cmake/CubismMverPhysics.cmake 用 #include 插进 SDK 的 CubismMotion.cpp,
 * 展开在 CubismMotion 的成员函数之间 —— 所以这里既不能有 include guard, 也不能用
 * namespace 包裹 (它已经在 SDK 的命名空间里了), 但可以直接访问 _motionData 这类私有成员。
 * 这样做的好处是 vendor 下的 SDK 保持原样: 升级 SDK 时只需重新核对 cmake 里的两个锚点字符串。
 *
 * 这里只改"贝塞尔段"的 Evaluate 函数指针, 其它段类型原样保留; 而且是对当前这一个动作对象
 * 逐次调用, 不是全局开关 —— 只有显式调用过它的动作才走 Mver 求值, 其余动作不受影响。
 */
void CubismMotion::UseMverCurveEvaluation()
{
    if (!_motionData)
    {
        return;
    }
    for (csmUint32 i = 0; i < _motionData->Segments.GetSize(); ++i)
    {
        if (_motionData->Segments[i].SegmentType == CubismMotionSegmentType_Bezier)
        {
            _motionData->Segments[i].Evaluate = BezierEvaluate;
        }
    }
}
