using System.Globalization;
using System.Windows;
using System.Windows.Data;

namespace ControlPanel
{
	// Shows an element only when the bound text is not empty (banners, warnings, messages),
	// or only when it is empty if Invert is set.
	public sealed class TextToVisibility : IValueConverter
	{
		public bool Invert { get; set; }

		public object Convert(object? value, Type targetType, object? parameter, CultureInfo culture) =>
			string.IsNullOrEmpty(value as string) != Invert ? Visibility.Collapsed : Visibility.Visible;

		public object ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture) =>
			Binding.DoNothing;
	}

	// Visible when the bound bool is true, or when it is false if Invert is set.
	public sealed class BoolToVisibility : IValueConverter
	{
		public bool Invert { get; set; }

		public object Convert(object? value, Type targetType, object? parameter, CultureInfo culture) =>
			(value is true) != Invert ? Visibility.Visible : Visibility.Collapsed;

		public object ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture) =>
			Binding.DoNothing;
	}

	// Visible when the bound count is 0 (for "nobody here yet" lines).
	public sealed class ZeroToVisibility : IValueConverter
	{
		public object Convert(object? value, Type targetType, object? parameter, CultureInfo culture) =>
			value is 0 ? Visibility.Visible : Visibility.Collapsed;

		public object ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture) =>
			Binding.DoNothing;
	}
}
