/*
 * 配置阶段由 cmake/CubismMverPhysics.cmake 用 #include 插进 SDK 的 CubismPhysics.cpp,
 * 展开在 CubismPhysics 的成员函数之间 —— 所以这里既不能有 include guard, 也不能用
 * namespace 包裹 (它已经在 SDK 的命名空间里了), 但可以直接使用 _physicsRig / _options /
 * UpdateParticles() 这些本类的私有成员, 这也是它必须被"插进来"而不是写成独立文件的原因。
 * 好处是 vendor 下的 SDK 保持原样: 升级 SDK 时只需重新核对 cmake 里的锚点字符串。
 *
 * 整段是 SDK Evaluate() 的 Mver 兼容变体: 按 SubRig 遍历, 先把输入参数归一化成平移与
 * 角度 (输入参数的索引是惰性解析的, -1 表示还没查过), 再旋转合成、推进粒子, 最后把结果
 * 写回输出参数。属性名与 SDK 的私有成员一一对应, 改名会在这里直接编译失败。
 */
void CubismPhysics::EvaluateMver(CubismModel* model, csmFloat32 deltaTimeSeconds)
{
    csmFloat32 totalAngle;
    csmFloat32 weight;
    csmFloat32 radAngle;
    csmFloat32 outputValue;
    CubismVector2 totalTranslation;
    csmInt32 i, settingIndex, particleIndex;
    CubismPhysicsSubRig* currentSetting;
    CubismPhysicsInput* currentInputs;
    CubismPhysicsOutput* currentOutputs;
    CubismPhysicsParticle* currentParticles;

    csmFloat32* parameterValues = Core::csmGetParameterValues(model->GetModel());
    const csmFloat32* parameterMaximumValues =
        Core::csmGetParameterMaximumValues(model->GetModel());
    const csmFloat32* parameterMinimumValues =
        Core::csmGetParameterMinimumValues(model->GetModel());
    const csmFloat32* parameterDefaultValues =
        Core::csmGetParameterDefaultValues(model->GetModel());

    for (settingIndex = 0; settingIndex < _physicsRig->SubRigCount; ++settingIndex)
    {
        totalAngle = 0.0f;
        totalTranslation.X = 0.0f;
        totalTranslation.Y = 0.0f;
        currentSetting = &_physicsRig->Settings[settingIndex];
        currentInputs = &_physicsRig->Inputs[currentSetting->BaseInputIndex];
        currentOutputs = &_physicsRig->Outputs[currentSetting->BaseOutputIndex];
        currentParticles = &_physicsRig->Particles[currentSetting->BaseParticleIndex];

        for (i = 0; i < currentSetting->InputCount; ++i)
        {
            weight = currentInputs[i].Weight / MaximumWeight;
            if (currentInputs[i].SourceParameterIndex == -1)
            {
                currentInputs[i].SourceParameterIndex =
                    model->GetParameterIndex(currentInputs[i].Source.Id);
            }
            currentInputs[i].GetNormalizedParameterValue(
                &totalTranslation,
                &totalAngle,
                parameterValues[currentInputs[i].SourceParameterIndex],
                parameterMinimumValues[currentInputs[i].SourceParameterIndex],
                parameterMaximumValues[currentInputs[i].SourceParameterIndex],
                parameterDefaultValues[currentInputs[i].SourceParameterIndex],
                &currentSetting->NormalizationPosition,
                &currentSetting->NormalizationAngle,
                currentInputs->Reflect,
                weight);
        }

        radAngle = CubismMath::DegreesToRadian(-totalAngle);
        totalTranslation.X = totalTranslation.X * CubismMath::CosF(radAngle)
            - totalTranslation.Y * CubismMath::SinF(radAngle);
        totalTranslation.Y = totalTranslation.X * CubismMath::SinF(radAngle)
            + totalTranslation.Y * CubismMath::CosF(radAngle);

        UpdateParticles(
            currentParticles,
            currentSetting->ParticleCount,
            totalTranslation,
            totalAngle,
            _options.Wind,
            MovementThreshold * currentSetting->NormalizationPosition.Maximum,
            deltaTimeSeconds,
            AirResistance);

        for (i = 0; i < currentSetting->OutputCount; ++i)
        {
            particleIndex = currentOutputs[i].VertexIndex;
            if (particleIndex < 1 || particleIndex >= currentSetting->ParticleCount)
            {
                break;
            }
            if (currentOutputs[i].DestinationParameterIndex == -1)
            {
                currentOutputs[i].DestinationParameterIndex =
                    model->GetParameterIndex(currentOutputs[i].Destination.Id);
            }

            CubismVector2 translation;
            translation.X = currentParticles[particleIndex].Position.X
                - currentParticles[particleIndex - 1].Position.X;
            translation.Y = currentParticles[particleIndex].Position.Y
                - currentParticles[particleIndex - 1].Position.Y;
            outputValue = currentOutputs[i].GetValue(
                translation,
                currentParticles,
                particleIndex,
                currentOutputs[i].Reflect,
                _options.Gravity);

            UpdateOutputParameterValue(
                &parameterValues[currentOutputs[i].DestinationParameterIndex],
                parameterMinimumValues[currentOutputs[i].DestinationParameterIndex],
                parameterMaximumValues[currentOutputs[i].DestinationParameterIndex],
                outputValue,
                &currentOutputs[i]);
        }
    }
}
