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
